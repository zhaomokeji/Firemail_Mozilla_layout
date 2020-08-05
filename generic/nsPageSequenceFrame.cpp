/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsPageSequenceFrame.h"

#include "mozilla/Logging.h"
#include "mozilla/PresShell.h"
#include "mozilla/dom/HTMLCanvasElement.h"
#include "mozilla/StaticPresData.h"

#include "DateTimeFormat.h"
#include "nsCOMPtr.h"
#include "nsDeviceContext.h"
#include "nsPresContext.h"
#include "gfxContext.h"
#include "nsGkAtoms.h"
#include "nsIFrame.h"
#include "nsIFrameInlines.h"
#include "nsIPrintSettings.h"
#include "nsPageFrame.h"
#include "nsSubDocumentFrame.h"
#include "nsRegion.h"
#include "nsCSSFrameConstructor.h"
#include "nsContentUtils.h"
#include "nsDisplayList.h"
#include "nsHTMLCanvasFrame.h"
#include "nsICanvasRenderingContextInternal.h"
#include "nsServiceManagerUtils.h"
#include <algorithm>

using namespace mozilla;
using namespace mozilla::dom;

mozilla::LazyLogModule gLayoutPrintingLog("printing-layout");

#define PR_PL(_p1) MOZ_LOG(gLayoutPrintingLog, mozilla::LogLevel::Debug, _p1)

nsPageSequenceFrame* NS_NewPageSequenceFrame(PresShell* aPresShell,
                                             ComputedStyle* aStyle) {
  return new (aPresShell)
      nsPageSequenceFrame(aStyle, aPresShell->GetPresContext());
}

NS_IMPL_FRAMEARENA_HELPERS(nsPageSequenceFrame)

nsPageSequenceFrame::nsPageSequenceFrame(ComputedStyle* aStyle,
                                         nsPresContext* aPresContext)
    : nsContainerFrame(aStyle, aPresContext, kClassID),
      mTotalPages(-1),
      mCalledBeginPage(false),
      mCurrentCanvasListSetup(false) {
  nscoord halfInch = PresContext()->CSSTwipsToAppUnits(NS_INCHES_TO_TWIPS(0.5));
  mMargin.SizeTo(halfInch, halfInch, halfInch, halfInch);

  mPageData = MakeUnique<nsSharedPageData>();
  mPageData->mHeadFootFont =
      *PresContext()
           ->Document()
           ->GetFontPrefsForLang(aStyle->StyleFont()->mLanguage)
           ->GetDefaultFont(StyleGenericFontFamily::Serif);
  mPageData->mHeadFootFont.size =
      Length::FromPixels(CSSPixel::FromPoints(10.0f));

  // Doing this here so we only have to go get these formats once
  SetPageNumberFormat("pagenumber", "%1$d", true);
  SetPageNumberFormat("pageofpages", "%1$d of %2$d", false);
}

nsPageSequenceFrame::~nsPageSequenceFrame() { ResetPrintCanvasList(); }

NS_QUERYFRAME_HEAD(nsPageSequenceFrame)
  NS_QUERYFRAME_ENTRY(nsPageSequenceFrame)
NS_QUERYFRAME_TAIL_INHERITING(nsContainerFrame)

//----------------------------------------------------------------------

float nsPageSequenceFrame::GetPrintPreviewScale() const {
  MOZ_DIAGNOSTIC_ASSERT(mAvailableISize >= 0, "Unset available width?");

  nsPresContext* pc = PresContext();
  float scale = pc->GetPrintPreviewScaleForSequenceFrame();
  if (pc->IsScreen()) {
    // For print preview, scale to the available size if needed.
    nscoord iSize = GetWritingMode().IsVertical() ? mSize.height : mSize.width;
    nscoord scaledISize = NSToCoordCeil(iSize * scale);
    if (scaledISize > mAvailableISize) {
      scale *= float(mAvailableISize) / float(scaledISize);
    }
  }
  return scale;
}

void nsPageSequenceFrame::PopulateReflowOutput(
    ReflowOutput& aReflowOutput, const ReflowInput& aReflowInput) {
  // Aim to fill the whole available space, not only so we can act as a
  // background in print preview but also handle overflow in child page frames
  // correctly.
  // Use availableISize so we don't cause a needless horizontal scrollbar.
  float scale = GetPrintPreviewScale();

  WritingMode wm = aReflowInput.GetWritingMode();
  nscoord iSize = wm.IsVertical() ? mSize.Height() : mSize.Width();
  nscoord bSize = wm.IsVertical() ? mSize.Width() : mSize.Height();

  aReflowOutput.ISize(wm) =
      std::max(NSToCoordFloor(iSize * scale), aReflowInput.AvailableISize());
  aReflowOutput.BSize(wm) =
      std::max(NSToCoordFloor(bSize * scale), aReflowInput.ComputedBSize());
  aReflowOutput.SetOverflowAreasToDesiredBounds();
}

// Helper function to compute the offset needed to center a child
// page-frame's margin-box inside our content-box.
nscoord nsPageSequenceFrame::ComputeCenteringMargin(
    nscoord aContainerContentBoxWidth, nscoord aChildPaddingBoxWidth,
    const nsMargin& aChildPhysicalMargin) {
  // We'll be centering our child's margin-box, so get the size of that:
  nscoord childMarginBoxWidth =
      aChildPaddingBoxWidth + aChildPhysicalMargin.LeftRight();

  // When rendered, our child's rect will actually be scaled up by the
  // print-preview scale factor, via ComputePageSequenceTransform().
  // We really want to center *that scaled-up rendering* inside of
  // aContainerContentBoxWidth.  So, we scale up its margin-box here...
  float scale = GetPrintPreviewScale();
  nscoord scaledChildMarginBoxWidth =
      NSToCoordRound(childMarginBoxWidth * scale);

  // ...and see we how much space is left over, when we subtract that scaled-up
  // size from the container width:
  nscoord scaledExtraSpace =
      aContainerContentBoxWidth - scaledChildMarginBoxWidth;

  if (scaledExtraSpace <= 0) {
    // (Don't bother centering if there's zero/negative space.)
    return 0;
  }

  // To center the child, we want to give it an additional left-margin of half
  // of the extra space.  And then, we have to scale that space back down, so
  // that it'll produce the correct scaled-up amount when we render (because
  // rendering will scale it back up):
  return NSToCoordRound(scaledExtraSpace * 0.5 / scale);
}

/*
 * Note: we largely position/size out our children (page frames) using
 * \*physical\* x/y/width/height values, because the print preview UI is always
 * arranged in the same orientation, regardless of writing mode.
 */
void nsPageSequenceFrame::Reflow(nsPresContext* aPresContext,
                                 ReflowOutput& aReflowOutput,
                                 const ReflowInput& aReflowInput,
                                 nsReflowStatus& aStatus) {
  MarkInReflow();
  MOZ_ASSERT(aPresContext->IsRootPaginatedDocument(),
             "A Page Sequence is only for real pages");
  DO_GLOBAL_REFLOW_COUNT("nsPageSequenceFrame");
  DISPLAY_REFLOW(aPresContext, this, aReflowInput, aReflowOutput, aStatus);
  MOZ_ASSERT(aStatus.IsEmpty(), "Caller should pass a fresh reflow status!");
  NS_FRAME_TRACE_REFLOW_IN("nsPageSequenceFrame::Reflow");

  auto CenterPages = [&] {
    for (nsIFrame* child : mFrames) {
      nsMargin pageCSSMargin = child->GetUsedMargin();
      nscoord centeringMargin =
          ComputeCenteringMargin(aReflowInput.ComputedWidth(),
                                 child->GetRect().Width(), pageCSSMargin);
      nscoord newX = pageCSSMargin.left + centeringMargin;

      // Adjust the child's x-position:
      child->MovePositionBy(nsPoint(newX - child->GetNormalPosition().x, 0));
    }
  };

  mAvailableISize = aReflowInput.AvailableISize();

  // Don't do incremental reflow until we've taught tables how to do
  // it right in paginated mode.
  if (!HasAnyStateBits(NS_FRAME_FIRST_REFLOW)) {
    // Return our desired size
    PopulateReflowOutput(aReflowOutput, aReflowInput);
    FinishAndStoreOverflow(&aReflowOutput);

    if (GetSize().Width() != aReflowOutput.Width()) {
      CenterPages();
    }
    return;
  }

  // See if we can get a Print Settings from the Context
  if (!mPageData->mPrintSettings &&
      aPresContext->Medium() == nsGkAtoms::print) {
    mPageData->mPrintSettings = aPresContext->GetPrintSettings();
  }

  // now get out margins & edges
  if (mPageData->mPrintSettings) {
    nsIntMargin unwriteableTwips;
    mPageData->mPrintSettings->GetUnwriteableMarginInTwips(unwriteableTwips);
    NS_ASSERTION(unwriteableTwips.left >= 0 && unwriteableTwips.top >= 0 &&
                     unwriteableTwips.right >= 0 &&
                     unwriteableTwips.bottom >= 0,
                 "Unwriteable twips should be non-negative");

    nsIntMargin marginTwips;
    mPageData->mPrintSettings->GetMarginInTwips(marginTwips);
    mMargin = nsPresContext::CSSTwipsToAppUnits(marginTwips + unwriteableTwips);

    int16_t printType;
    mPageData->mPrintSettings->GetPrintRange(&printType);
    mPrintRangeType = printType;

    nsIntMargin edgeTwips;
    mPageData->mPrintSettings->GetEdgeInTwips(edgeTwips);

    // sanity check the values. three inches are sometimes needed
    int32_t inchInTwips = NS_INCHES_TO_INT_TWIPS(3.0);
    edgeTwips.top = clamped(edgeTwips.top, 0, inchInTwips);
    edgeTwips.bottom = clamped(edgeTwips.bottom, 0, inchInTwips);
    edgeTwips.left = clamped(edgeTwips.left, 0, inchInTwips);
    edgeTwips.right = clamped(edgeTwips.right, 0, inchInTwips);

    mPageData->mEdgePaperMargin =
        nsPresContext::CSSTwipsToAppUnits(edgeTwips + unwriteableTwips);
  }

  // *** Special Override ***
  // If this is a sub-sdoc (meaning it doesn't take the whole page)
  // and if this Document is in the upper left hand corner
  // we need to suppress the top margin or it will reflow too small

  nsSize pageSize = aPresContext->GetPageSize();

  mPageData->mReflowSize = pageSize;
  mPageData->mReflowMargin = mMargin;

  // We use the CSS "margin" property on the -moz-page pseudoelement
  // to determine the space between each page in print preview.
  // Keep a running y-offset for each page.
  nscoord y = 0;
  nscoord maxXMost = 0;

  // Tile the pages vertically
  for (nsIFrame* kidFrame : mFrames) {
    // Set the shared data into the page frame before reflow
    auto* pf = static_cast<nsPageFrame*>(kidFrame);
    pf->SetSharedPageData(mPageData.get());

    // Reflow the page
    ReflowInput kidReflowInput(
        aPresContext, aReflowInput, kidFrame,
        LogicalSize(kidFrame->GetWritingMode(), pageSize));
    ReflowOutput kidReflowOutput(kidReflowInput);
    nsReflowStatus status;

    kidReflowInput.SetComputedISize(kidReflowInput.AvailableISize());
    // kidReflowInput.SetComputedHeight(kidReflowInput.AvailableHeight());
    PR_PL(("AV ISize: %d   BSize: %d\n", kidReflowInput.AvailableISize(),
           kidReflowInput.AvailableBSize()));

    nsMargin pageCSSMargin = kidReflowInput.ComputedPhysicalMargin();
    y += pageCSSMargin.top;

    nscoord x = pageCSSMargin.left;

    // Place and size the page.
    ReflowChild(kidFrame, aPresContext, kidReflowOutput, kidReflowInput, x, y,
                ReflowChildFlags::Default, status);

    FinishReflowChild(kidFrame, aPresContext, kidReflowOutput, &kidReflowInput,
                      x, y, ReflowChildFlags::Default);
    y += kidReflowOutput.Height();
    y += pageCSSMargin.bottom;

    maxXMost =
        std::max(maxXMost, x + kidReflowOutput.Width() + pageCSSMargin.right);

    // Is the page complete?
    nsIFrame* kidNextInFlow = kidFrame->GetNextInFlow();

    if (status.IsFullyComplete()) {
      NS_ASSERTION(!kidNextInFlow, "bad child flow list");
    } else if (!kidNextInFlow) {
      // The page isn't complete and it doesn't have a next-in-flow, so
      // create a continuing page.
      nsIFrame* continuingPage =
          PresShell()->FrameConstructor()->CreateContinuingFrame(kidFrame,
                                                                 this);

      // Add it to our child list
      mFrames.InsertFrame(nullptr, kidFrame, continuingPage);
    }
  }

  // Get Total Page Count
  // XXXdholbert technically we could calculate this in the loop above,
  // instead of needing a separate walk.
  int32_t pageTot = mFrames.GetLength();

  // Set Page Number Info
  int32_t pageNum = 1;
  for (nsIFrame* child : mFrames) {
    MOZ_ASSERT(child->IsPageFrame(),
               "only expecting nsPageFrame children. Other children will make "
               "this static_cast bogus & probably violate other assumptions");
    auto* pf = static_cast<nsPageFrame*>(child);
    pf->SetPageNumInfo(pageNum, pageTot);
    pageNum++;
  }

  nsAutoString formattedDateString;
  PRTime now = PR_Now();
  if (NS_SUCCEEDED(DateTimeFormat::FormatPRTime(
          kDateFormatShort, kTimeFormatNoSeconds, now, formattedDateString))) {
    SetDateTimeStr(formattedDateString);
  }

  // cache the size so we can set the desired size
  // for the other reflows that happen
  mSize = nsSize(maxXMost, y);

  // Return our desired size
  // Adjust the reflow size by PrintPreviewScale so the scrollbars end up the
  // correct size
  PopulateReflowOutput(aReflowOutput, aReflowInput);

  FinishAndStoreOverflow(&aReflowOutput);

  // Now center our pages.
  CenterPages();

  NS_FRAME_TRACE_REFLOW_OUT("nsPageSequenceFrame::Reflow", aStatus);
  NS_FRAME_SET_TRUNCATION(aStatus, aReflowInput, aReflowOutput);
}

//----------------------------------------------------------------------

#ifdef DEBUG_FRAME_DUMP
nsresult nsPageSequenceFrame::GetFrameName(nsAString& aResult) const {
  return MakeFrameName(u"PageSequence"_ns, aResult);
}
#endif

//====================================================================
//== Asynch Printing
//====================================================================
void nsPageSequenceFrame::GetPrintRange(int32_t* aFromPage,
                                        int32_t* aToPage) const {
  *aFromPage = mFromPageNum;
  *aToPage = mToPageNum;
}

// Helper Function
void nsPageSequenceFrame::SetPageNumberFormat(const char* aPropName,
                                              const char* aDefPropVal,
                                              bool aPageNumOnly) {
  // Doing this here so we only have to go get these formats once
  nsAutoString pageNumberFormat;
  // Now go get the Localized Page Formating String
  nsresult rv = nsContentUtils::GetLocalizedString(
      nsContentUtils::ePRINTING_PROPERTIES, aPropName, pageNumberFormat);
  if (NS_FAILED(rv)) {  // back stop formatting
    pageNumberFormat.AssignASCII(aDefPropVal);
  }

  SetPageNumberFormat(pageNumberFormat, aPageNumOnly);
}

nsresult nsPageSequenceFrame::StartPrint(nsPresContext* aPresContext,
                                         nsIPrintSettings* aPrintSettings,
                                         const nsAString& aDocTitle,
                                         const nsAString& aDocURL) {
  NS_ENSURE_ARG_POINTER(aPresContext);
  NS_ENSURE_ARG_POINTER(aPrintSettings);

  if (!mPageData->mPrintSettings) {
    mPageData->mPrintSettings = aPrintSettings;
  }

  if (!aDocTitle.IsEmpty()) {
    mPageData->mDocTitle = aDocTitle;
  }
  if (!aDocURL.IsEmpty()) {
    mPageData->mDocURL = aDocURL;
  }

  aPrintSettings->GetStartPageRange(&mFromPageNum);
  aPrintSettings->GetEndPageRange(&mToPageNum);
  aPrintSettings->GetPageRanges(mPageRanges);

  mDoingPageRange =
      nsIPrintSettings::kRangeSpecifiedPageRange == mPrintRangeType;

  // If printing a range of pages make sure at least the starting page
  // number is valid
  mTotalPages = mFrames.GetLength();

  if (mDoingPageRange) {
    if (mFromPageNum > mTotalPages) {
      return NS_ERROR_INVALID_ARG;
    }
  }

  // Begin printing of the document
  mPageNum = 1;
  return NS_OK;
}

static void GetPrintCanvasElementsInFrame(
    nsIFrame* aFrame, nsTArray<RefPtr<HTMLCanvasElement> >* aArr) {
  if (!aFrame) {
    return;
  }
  for (const auto& childList : aFrame->ChildLists()) {
    for (nsIFrame* child : childList.mList) {
      // Check if child is a nsHTMLCanvasFrame.
      nsHTMLCanvasFrame* canvasFrame = do_QueryFrame(child);

      // If there is a canvasFrame, try to get actual canvas element.
      if (canvasFrame) {
        HTMLCanvasElement* canvas =
            HTMLCanvasElement::FromNodeOrNull(canvasFrame->GetContent());
        if (canvas && canvas->GetMozPrintCallback()) {
          aArr->AppendElement(canvas);
          continue;
        }
      }

      if (!child->PrincipalChildList().FirstChild()) {
        nsSubDocumentFrame* subdocumentFrame = do_QueryFrame(child);
        if (subdocumentFrame) {
          // Descend into the subdocument
          nsIFrame* root = subdocumentFrame->GetSubdocumentRootFrame();
          child = root;
        }
      }
      // The current child is not a nsHTMLCanvasFrame OR it is but there is
      // no HTMLCanvasElement on it. Check if children of `child` might
      // contain a HTMLCanvasElement.
      GetPrintCanvasElementsInFrame(child, aArr);
    }
  }
}

void nsPageSequenceFrame::DetermineWhetherToPrintPage() {
  // See whether we should print this page
  mPrintThisPage = true;
  bool printEvenPages, printOddPages;
  mPageData->mPrintSettings->GetPrintOptions(nsIPrintSettings::kPrintEvenPages,
                                             &printEvenPages);
  mPageData->mPrintSettings->GetPrintOptions(nsIPrintSettings::kPrintOddPages,
                                             &printOddPages);

  // If printing a range of pages check whether the page number is in the
  // range of pages to print
  if (mDoingPageRange) {
    if (mPageNum < mFromPageNum) {
      mPrintThisPage = false;
    } else if (mPageNum > mToPageNum) {
      mPageNum++;
      mPrintThisPage = false;
      return;
    } else {
      int32_t length = mPageRanges.Length();

      // Page ranges are pairs (start, end)
      if (length && (length % 2 == 0)) {
        mPrintThisPage = false;

        int32_t i;
        for (i = 0; i < length; i += 2) {
          if (mPageRanges[i] <= mPageNum && mPageNum <= mPageRanges[i + 1]) {
            mPrintThisPage = true;
            break;
          }
        }
      }
    }
  }

  // Check for printing of odd and even pages
  if (mPageNum & 0x1) {
    if (!printOddPages) {
      mPrintThisPage = false;  // don't print odd numbered page
    }
  } else {
    if (!printEvenPages) {
      mPrintThisPage = false;  // don't print even numbered page
    }
  }
}

nsIFrame* nsPageSequenceFrame::GetCurrentPageFrame() {
  int32_t i = 1;
  for (nsIFrame* child : mFrames) {
    if (i == mPageNum) {
      return child;
    }
    ++i;
  }
  return nullptr;
}

nsresult nsPageSequenceFrame::PrePrintNextPage(nsITimerCallback* aCallback,
                                               bool* aDone) {
  nsIFrame* currentPage = GetCurrentPageFrame();
  if (!currentPage) {
    *aDone = true;
    return NS_ERROR_FAILURE;
  }

  DetermineWhetherToPrintPage();
  // Nothing to do if the current page doesn't get printed OR rendering to
  // preview. For preview, the `CallPrintCallback` is called from within the
  // HTMLCanvasElement::HandlePrintCallback.
  if (!mPrintThisPage || !PresContext()->IsRootPaginatedDocument()) {
    *aDone = true;
    return NS_OK;
  }

  // If the canvasList is null, then generate it and start the render
  // process for all the canvas.
  if (!mCurrentCanvasListSetup) {
    mCurrentCanvasListSetup = true;
    GetPrintCanvasElementsInFrame(currentPage, &mCurrentCanvasList);

    if (mCurrentCanvasList.Length() != 0) {
      nsresult rv = NS_OK;

      // Begin printing of the document
      nsDeviceContext* dc = PresContext()->DeviceContext();
      PR_PL(("\n"));
      PR_PL(("***************** BeginPage *****************\n"));
      rv = dc->BeginPage();
      NS_ENSURE_SUCCESS(rv, rv);

      mCalledBeginPage = true;

      RefPtr<gfxContext> renderingContext = dc->CreateRenderingContext();
      NS_ENSURE_TRUE(renderingContext, NS_ERROR_OUT_OF_MEMORY);

      DrawTarget* drawTarget = renderingContext->GetDrawTarget();
      if (NS_WARN_IF(!drawTarget)) {
        return NS_ERROR_FAILURE;
      }

      for (int32_t i = mCurrentCanvasList.Length() - 1; i >= 0; i--) {
        HTMLCanvasElement* canvas = mCurrentCanvasList[i];
        nsIntSize size = canvas->GetSize();

        RefPtr<DrawTarget> canvasTarget =
            drawTarget->CreateSimilarDrawTarget(size, drawTarget->GetFormat());
        if (!canvasTarget) {
          continue;
        }

        nsICanvasRenderingContextInternal* ctx = canvas->GetContextAtIndex(0);
        if (!ctx) {
          continue;
        }

        // Initialize the context with the new DrawTarget.
        ctx->InitializeWithDrawTarget(nullptr, WrapNotNull(canvasTarget));

        // Start the rendering process.
        // Note: Other than drawing to our CanvasRenderingContext2D, the
        // callback cannot access or mutate our static clone document.  It is
        // evaluated in its original context (the window of the original
        // document) of course, and our canvas has a strong ref to the
        // original HTMLCanvasElement (in mOriginalCanvas) so that if the
        // callback calls GetCanvas() on our CanvasRenderingContext2D (passed
        // to it via a MozCanvasPrintState argument) it will be given the
        // original 'canvas' element.
        AutoWeakFrame weakFrame = this;
        canvas->DispatchPrintCallback(aCallback);
        NS_ENSURE_STATE(weakFrame.IsAlive());
      }
    }
  }
  uint32_t doneCounter = 0;
  for (int32_t i = mCurrentCanvasList.Length() - 1; i >= 0; i--) {
    HTMLCanvasElement* canvas = mCurrentCanvasList[i];

    if (canvas->IsPrintCallbackDone()) {
      doneCounter++;
    }
  }
  // If all canvas have finished rendering, return true, otherwise false.
  *aDone = doneCounter == mCurrentCanvasList.Length();

  return NS_OK;
}

void nsPageSequenceFrame::ResetPrintCanvasList() {
  for (int32_t i = mCurrentCanvasList.Length() - 1; i >= 0; i--) {
    HTMLCanvasElement* canvas = mCurrentCanvasList[i];
    canvas->ResetPrintCallback();
  }

  mCurrentCanvasList.Clear();
  mCurrentCanvasListSetup = false;
}

nsresult nsPageSequenceFrame::PrintNextPage() {
  // Note: When print al the pages or a page range the printed page shows the
  // actual page number, when printing selection it prints the page number
  // starting with the first page of the selection. For example if the user has
  // a selection that starts on page 2 and ends on page 3, the page numbers when
  // print are 1 and then two (which is different than printing a page range,
  // where the page numbers would have been 2 and then 3)

  nsIFrame* currentPageFrame = GetCurrentPageFrame();
  if (!currentPageFrame) {
    return NS_ERROR_FAILURE;
  }

  nsresult rv = NS_OK;

  DetermineWhetherToPrintPage();

  if (mPrintThisPage) {
    nsDeviceContext* dc = PresContext()->DeviceContext();

    if (PresContext()->IsRootPaginatedDocument()) {
      if (!mCalledBeginPage) {
        // We must make sure BeginPage() has been called since some printing
        // backends can't give us a valid rendering context for a [physical]
        // page otherwise.
        PR_PL(("\n"));
        PR_PL(("***************** BeginPage *****************\n"));
        rv = dc->BeginPage();
        NS_ENSURE_SUCCESS(rv, rv);
      }
    }

    PR_PL(
        ("SeqFr::PrintNextPage -> %p PageNo: %d", currentPageFrame, mPageNum));

    // CreateRenderingContext can fail
    RefPtr<gfxContext> gCtx = dc->CreateRenderingContext();
    NS_ENSURE_TRUE(gCtx, NS_ERROR_OUT_OF_MEMORY);

    nsRect drawingRect(nsPoint(0, 0), currentPageFrame->GetSize());
    nsRegion drawingRegion(drawingRect);
    nsLayoutUtils::PaintFrame(gCtx, currentPageFrame, drawingRegion,
                              NS_RGBA(0, 0, 0, 0),
                              nsDisplayListBuilderMode::Painting,
                              nsLayoutUtils::PaintFrameFlags::SyncDecodeImages);
  }
  return rv;
}

nsresult nsPageSequenceFrame::DoPageEnd() {
  nsresult rv = NS_OK;
  if (PresContext()->IsRootPaginatedDocument() && mPrintThisPage) {
    PR_PL(("***************** End Page (DoPageEnd) *****************\n"));
    rv = PresContext()->DeviceContext()->EndPage();
    NS_ENSURE_SUCCESS(rv, rv);
  }

  ResetPrintCanvasList();
  mCalledBeginPage = false;

  mPageNum++;

  return rv;
}

gfx::Matrix4x4 ComputePageSequenceTransform(nsIFrame* aFrame,
                                            float aAppUnitsPerPixel) {
  MOZ_ASSERT(aFrame->IsPageSequenceFrame());
  float scale =
      static_cast<nsPageSequenceFrame*>(aFrame)->GetPrintPreviewScale();
  return gfx::Matrix4x4::Scaling(scale, scale, 1);
}

void nsPageSequenceFrame::BuildDisplayList(nsDisplayListBuilder* aBuilder,
                                           const nsDisplayListSet& aLists) {
  aBuilder->SetInPageSequence(true);
  aBuilder->SetDisablePartialUpdates(true);
  DisplayBorderBackgroundOutline(aBuilder, aLists);

  nsDisplayList content;

  {
    // Clear clip state while we construct the children of the
    // nsDisplayTransform, since they'll be in a different coordinate system.
    DisplayListClipState::AutoSaveRestore clipState(aBuilder);
    clipState.Clear();

    nsIFrame* child = PrincipalChildList().FirstChild();
    nsRect visible = aBuilder->GetVisibleRect();
    visible.ScaleInverseRoundOut(GetPrintPreviewScale());

    while (child) {
      if (child->InkOverflowRectRelativeToParent().Intersects(visible)) {
        nsDisplayListBuilder::AutoBuildingDisplayList buildingForChild(
            aBuilder, child, visible - child->GetPosition(),
            visible - child->GetPosition());
        child->BuildDisplayListForStackingContext(aBuilder, &content);
        aBuilder->ResetMarkedFramesForDisplayList(this);
      }
      child = child->GetNextSibling();
    }
  }

  content.AppendNewToTop<nsDisplayTransform>(aBuilder, this, &content,
                                             content.GetBuildingRect(),
                                             ::ComputePageSequenceTransform);

  aLists.Content()->AppendToTop(&content);
  aBuilder->SetInPageSequence(false);
}

//------------------------------------------------------------------------------
void nsPageSequenceFrame::SetPageNumberFormat(const nsAString& aFormatStr,
                                              bool aForPageNumOnly) {
  NS_ASSERTION(mPageData != nullptr, "mPageData string cannot be null!");

  if (aForPageNumOnly) {
    mPageData->mPageNumFormat = aFormatStr;
  } else {
    mPageData->mPageNumAndTotalsFormat = aFormatStr;
  }
}

//------------------------------------------------------------------------------
void nsPageSequenceFrame::SetDateTimeStr(const nsAString& aDateTimeStr) {
  NS_ASSERTION(mPageData != nullptr, "mPageData string cannot be null!");

  mPageData->mDateTimeStr = aDateTimeStr;
}

void nsPageSequenceFrame::AppendDirectlyOwnedAnonBoxes(
    nsTArray<OwnedAnonBox>& aResult) {
  if (mFrames.NotEmpty()) {
    aResult.AppendElement(mFrames.FirstChild());
  }
}

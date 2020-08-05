/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* base class for representation of media lists */

#include "mozilla/dom/MediaList.h"

#include "mozAutoDocUpdate.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/MediaListBinding.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/ServoStyleSet.h"
#include "mozilla/StyleSheetInlines.h"

namespace mozilla {
namespace dom {

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MediaList)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(MediaList)
NS_IMPL_CYCLE_COLLECTING_RELEASE(MediaList)

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_0(MediaList)

JSObject* MediaList::WrapObject(JSContext* aCx,
                                JS::Handle<JSObject*> aGivenProto) {
  return MediaList_Binding::Wrap(aCx, this, aGivenProto);
}

void MediaList::SetStyleSheet(StyleSheet* aSheet) {
  MOZ_ASSERT(aSheet == mStyleSheet || !aSheet || !mStyleSheet,
             "Multiple style sheets competing for one media list");
  mStyleSheet = aSheet;
}

template <typename Func>
void MediaList::DoMediaChange(Func aCallback, ErrorResult& aRv) {
  if (IsReadOnly()) {
    return;
  }

  if (mStyleSheet) {
    mStyleSheet->WillDirty();
  }

  aCallback(aRv);
  if (aRv.Failed()) {
    return;
  }

  if (mStyleSheet) {
    // FIXME(emilio): We should discern between "owned by a rule" (as in @media)
    // and "owned by a sheet" (as in <style media>), and then pass something
    // meaningful here.
    mStyleSheet->RuleChanged(nullptr);
  }
}

already_AddRefed<MediaList> MediaList::Clone() {
  RefPtr<MediaList> clone =
      new MediaList(Servo_MediaList_DeepClone(mRawList).Consume());
  return clone.forget();
}

MediaList::MediaList() : mRawList(Servo_MediaList_Create().Consume()) {}

MediaList::MediaList(const nsAString& aMedia, CallerType aCallerType)
    : MediaList() {
  SetTextInternal(aMedia, aCallerType);
}

void MediaList::GetText(nsAString& aMediaText) {
  Servo_MediaList_GetText(mRawList, &aMediaText);
}

/* static */
already_AddRefed<MediaList> MediaList::Create(const nsAString& aMedia,
                                              CallerType aCallerType) {
  RefPtr<MediaList> mediaList = new MediaList(aMedia, aCallerType);
  return mediaList.forget();
}

void MediaList::SetText(const nsAString& aMediaText) {
  if (IsReadOnly()) {
    return;
  }

  SetTextInternal(aMediaText, CallerType::NonSystem);
}

void MediaList::GetMediaText(nsAString& aMediaText) { GetText(aMediaText); }

void MediaList::SetTextInternal(const nsAString& aMediaText,
                                CallerType aCallerType) {
  NS_ConvertUTF16toUTF8 mediaText(aMediaText);
  Servo_MediaList_SetText(mRawList, &mediaText, aCallerType);
}

uint32_t MediaList::Length() { return Servo_MediaList_GetLength(mRawList); }

void MediaList::IndexedGetter(uint32_t aIndex, bool& aFound,
                              nsAString& aReturn) {
  aFound = Servo_MediaList_GetMediumAt(mRawList, aIndex, &aReturn);
  if (!aFound) {
    SetDOMStringToNull(aReturn);
  }
}

void MediaList::Delete(const nsAString& aOldMedium, ErrorResult& aRv) {
  MOZ_ASSERT(!IsReadOnly());
  NS_ConvertUTF16toUTF8 oldMedium(aOldMedium);
  if (Servo_MediaList_DeleteMedium(mRawList, &oldMedium)) {
    return;
  }
  aRv.ThrowNotFoundError("Medium not in list");
}

bool MediaList::Matches(const Document& aDocument) const {
  const RawServoStyleSet* rawSet =
      aDocument.StyleSetForPresShellOrMediaQueryEvaluation()->RawSet();
  MOZ_ASSERT(rawSet, "The RawServoStyleSet should be valid!");
  return Servo_MediaList_Matches(mRawList, rawSet);
}

void MediaList::Append(const nsAString& aNewMedium, ErrorResult& aRv) {
  MOZ_ASSERT(!IsReadOnly());
  if (aNewMedium.IsEmpty()) {
    // XXXbz per spec there should not be an exception here, as far as
    // I can tell...
    aRv.ThrowNotFoundError("Empty medium");
    return;
  }
  NS_ConvertUTF16toUTF8 newMedium(aNewMedium);
  Servo_MediaList_AppendMedium(mRawList, &newMedium);
}

void MediaList::SetMediaText(const nsAString& aMediaText) {
  DoMediaChange([&](ErrorResult& aRv) { SetText(aMediaText); }, IgnoreErrors());
}

void MediaList::Item(uint32_t aIndex, nsAString& aReturn) {
  bool dummy;
  IndexedGetter(aIndex, dummy, aReturn);
}

void MediaList::DeleteMedium(const nsAString& aOldMedium, ErrorResult& aRv) {
  DoMediaChange([&](ErrorResult& aRv) { Delete(aOldMedium, aRv); }, aRv);
}

void MediaList::AppendMedium(const nsAString& aNewMedium, ErrorResult& aRv) {
  DoMediaChange([&](ErrorResult& aRv) { Append(aNewMedium, aRv); }, aRv);
}

MOZ_DEFINE_MALLOC_SIZE_OF(ServoMediaListMallocSizeOf)
MOZ_DEFINE_MALLOC_ENCLOSING_SIZE_OF(ServoMediaListMallocEnclosingSizeOf)

size_t MediaList::SizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const {
  size_t n = 0;
  n += Servo_MediaList_SizeOfIncludingThis(ServoMediaListMallocSizeOf,
                                           ServoMediaListMallocEnclosingSizeOf,
                                           mRawList);
  return n;
}

bool MediaList::IsReadOnly() const {
  return mStyleSheet && mStyleSheet->IsReadOnly();
}

}  // namespace dom
}  // namespace mozilla

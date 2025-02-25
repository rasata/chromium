// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_UI_OMNIBOX_POPUP_AUTOCOMPLETE_MATCH_FORMATTER_H_
#define IOS_CHROME_BROWSER_UI_OMNIBOX_POPUP_AUTOCOMPLETE_MATCH_FORMATTER_H_

#import "ios/chrome/browser/ui/omnibox/popup/autocomplete_suggestion.h"

struct AutocompleteMatch;
@interface AutocompleteMatchFormatter : NSObject <AutocompleteSuggestion>

// This is a temporary solution for coloring strings.
@property(nonatomic, assign, getter=isIncognito) BOOL incognito;
@property(nonatomic, assign, getter=isStarred) BOOL starred;

- (instancetype)initWithMatch:(const AutocompleteMatch&)match
    NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

// Convenience constuctor.
+ (instancetype)formatterWithMatch:(const AutocompleteMatch&)match;

@end

#endif  // IOS_CHROME_BROWSER_UI_OMNIBOX_POPUP_AUTOCOMPLETE_MATCH_FORMATTER_H_

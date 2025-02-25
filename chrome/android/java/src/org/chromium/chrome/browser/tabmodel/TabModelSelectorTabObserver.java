// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tabmodel;

import android.util.SparseArray;

import org.chromium.chrome.browser.tab.EmptyTabObserver;
import org.chromium.chrome.browser.tab.Tab;

import java.util.List;

/**
 * Observer of tab changes for all tabs owned by a {@link TabModelSelector}.
 */
public class TabModelSelectorTabObserver extends EmptyTabObserver {

    private final TabModelSelector mTabModelSelector;
    private final TabModelSelectorTabModelObserver mTabModelObserver;
    private final SparseArray<Tab> mTabsToClose = new SparseArray<>();

    /**
     * Constructs an observer that should be notified of tabs changes for all tabs owned
     * by a specified {@link TabModelSelector}.  Any Tabs created after this call will be
     * observed as well, and Tabs removed will no longer have their information broadcast.
     *
     * <p>
     * {@link #destroy()} must be called to unregister this observer.
     *
     * @param selector The selector that owns the Tabs that should notify this observer.
     */
    public TabModelSelectorTabObserver(TabModelSelector selector) {
        mTabModelSelector = selector;

        mTabModelObserver = new TabModelSelectorTabModelObserver(selector) {
            @Override
            public void didAddTab(Tab tab, @TabLaunchType int type) {
                // This observer is automatically removed by tab when it is destroyed.
                tab.addObserver(TabModelSelectorTabObserver.this);
                onTabRegistered(tab);
            }

            @Override
            public void willCloseTab(Tab tab, boolean animate) {
                mTabsToClose.put(tab.getId(), tab);
            }

            @Override
            public void tabClosureUndone(Tab tab) {
                mTabsToClose.remove(tab.getId());
            }

            @Override
            public void didCloseTab(int tabId, boolean incognito) {
                Tab tab = mTabsToClose.get(tabId);
                if (tab != null) {
                    mTabsToClose.remove(tabId);
                    onTabUnregistered(tab);
                }
            }

            @Override
            public void tabRemoved(Tab tab) {
                tab.removeObserver(TabModelSelectorTabObserver.this);
                onTabUnregistered(tab);
            }

            @Override
            protected void onRegistrationComplete() {
                List<TabModel> tabModels = mTabModelSelector.getModels();
                for (int i = 0; i < tabModels.size(); i++) {
                    TabModel tabModel = tabModels.get(i);
                    TabList comprehensiveTabList = tabModel.getComprehensiveModel();
                    for (int j = 0; j < comprehensiveTabList.getCount(); j++) {
                        Tab tab = comprehensiveTabList.getTabAt(j);
                        tab.addObserver(TabModelSelectorTabObserver.this);
                        onTabRegistered(tab);
                    }
                }
            }
        };
    }

    /**
     * Called when a tab is registered to a tab model this selector is managing.
     * @param tab The registered Tab.
     */
    protected void onTabRegistered(Tab tab) {}

    /**
     * Called when a tab is unregistered from a tab model this selector is managing.
     * @param tab The unregistered Tab.
     */
    protected void onTabUnregistered(Tab tab) {}

    /**
     * Destroys the observer and removes itself as a listener for Tab updates.
     */
    public void destroy() {
        mTabModelObserver.destroy();

        List<TabModel> tabModels = mTabModelSelector.getModels();
        for (int i = 0; i < tabModels.size(); i++) {
            TabModel tabModel = tabModels.get(i);
            tabModel.removeObserver(mTabModelObserver);

            TabList comprehensiveTabList = tabModel.getComprehensiveModel();
            for (int j = 0; j < comprehensiveTabList.getCount(); j++) {
                Tab tab = comprehensiveTabList.getTabAt(j);
                tab.removeObserver(this);
                onTabUnregistered(tab);
            }
        }
    }
}

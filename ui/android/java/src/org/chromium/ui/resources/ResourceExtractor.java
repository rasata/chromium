// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.ui.resources;

import android.content.res.AssetManager;

import org.chromium.base.BuildConfig;
import org.chromium.base.BuildInfo;
import org.chromium.base.ContextUtils;
import org.chromium.base.FileUtils;
import org.chromium.base.LocaleUtils;
import org.chromium.base.Log;
import org.chromium.base.PathUtils;
import org.chromium.base.ThreadUtils;
import org.chromium.base.TraceEvent;
import org.chromium.base.task.PostTask;
import org.chromium.base.task.TaskTraits;
import org.chromium.ui.base.LocalizationUtils;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;

/**
 * Handles extracting the necessary resources bundled in an APK and moving them to a location on
 * the file system accessible from the native code.
 */
public class ResourceExtractor {
    private static final String TAG = "ui";
    private static final String ICU_DATA_FILENAME = "icudtl.dat";
    private static final String V8_NATIVES_DATA_FILENAME = "natives_blob.bin";
    private static final String V8_SNAPSHOT_DATA_FILENAME = "snapshot_blob.bin";
    private static final String FALLBACK_LOCALE = "en-US";
    private static final String COMPRESSED_LOCALES_DIR = "locales";
    private static final int BUFFER_SIZE = 16 * 1024;
    private static final boolean DEBUG = false;

    private class ExtractTask implements Runnable {
        private final List<Runnable> mCompletionCallbacks = new ArrayList<Runnable>();
        private final String mUiLanguage;
        private final CountDownLatch mLatch = new CountDownLatch(1);
        private boolean mDone;

        public ExtractTask(String uiLanguage) {
            mUiLanguage = uiLanguage;
        }

        @Override
        public void run() {
            try (TraceEvent e = TraceEvent.scoped("ResourceExtractor.ExtractTask.doInBackground")) {
                doInBackgroundImpl();
            }
            synchronized (this) {
                mDone = true;
            }
            mLatch.countDown();

            PostTask.postTask(mResultTaskTraits, () -> {
                try (TraceEvent e =
                                TraceEvent.scoped("ResourceExtractor.ExtractTask.onPostExecute")) {
                    onPostExecuteImpl();
                }
            });
        }

        private void doInBackgroundImpl() {
            final File outputDir = getOutputDir();
            String[] assetPaths = detectFilesToExtract(mUiLanguage);

            // Use a suffix for extracted files in order to guarantee that the version of the file
            // on disk matches up with the version of the APK.
            String extractSuffix = BuildInfo.getInstance().extractedFileSuffix;
            String[] outputNames = new String[assetPaths.length];
            for (int n = 0; n < assetPaths.length; ++n) {
                String assetPath = assetPaths[n];
                outputNames[n] =
                        assetPath.substring(assetPath.lastIndexOf('/') + 1) + extractSuffix;
            }

            String[] existingFileNames = outputDir.list();
            boolean allFilesExist = existingFileNames != null;
            if (allFilesExist) {
                List<String> existingFiles = Arrays.asList(existingFileNames);
                for (String outputName : outputNames) {
                    allFilesExist &= existingFiles.contains(outputName);
                }
            }
            // This is the normal case.
            if (allFilesExist) {
                return;
            }
            // A missing file means Chrome has updated. Delete stale files first.
            deleteFiles(existingFileNames);

            outputDir.mkdirs();
            if (!outputDir.exists()) {
                // Return value of mkdirs() sometimes incorrect? https://crbug.com/849550
                throw new RuntimeException();
            }

            AssetManager assetManager = ContextUtils.getApplicationAssets();
            byte[] buffer = new byte[BUFFER_SIZE];
            for (int n = 0; n < assetPaths.length; ++n) {
                String assetPath = assetPaths[n];
                File output = new File(outputDir, outputNames[n]);
                TraceEvent.begin("ExtractResource");
                try (InputStream inputStream = assetManager.open(assetPath)) {
                    FileUtils.copyFileStreamAtomicWithBuffer(inputStream, output, buffer);
                } catch (IOException e) {
                    // The app would just crash later if files are missing.
                    throw new RuntimeException(e);
                } finally {
                    TraceEvent.end("ExtractResource");
                }
            }
        }

        private void onPostExecuteImpl() {
            ThreadUtils.assertOnUiThread();
            for (int i = 0; i < mCompletionCallbacks.size(); i++) {
                mCompletionCallbacks.get(i).run();
            }
            mCompletionCallbacks.clear();
        }

        public void await() throws Exception {
            mLatch.await();
        }

        public synchronized boolean isDone() {
            return mDone;
        }
    }

    private ExtractTask mExtractTask;
    private TaskTraits mResultTaskTraits;

    private static ResourceExtractor sInstance;

    public static ResourceExtractor get() {
        if (sInstance == null) {
            sInstance = new ResourceExtractor();
        }
        return sInstance;
    }

    private static String[] detectFilesToExtract(String uiLanguage) {
        Locale defaultLocale = Locale.getDefault();
        String androidLanguage = defaultLocale.getLanguage();
        String chromiumLanguage = LocaleUtils.getUpdatedLanguageForChromium(androidLanguage);

        // NOTE: The UI language will differ from the application's language
        // when the system locale is not directly supported by Chrome's
        // resources.
        if (DEBUG) {
            Log.i(TAG, "Using UI language %s, system language: %s (Android name: %s)", uiLanguage,
                    chromiumLanguage, androidLanguage);
        }

        // Currenty (Apr 2018), this array can be as big as 6 entries, so using a capacity
        // that allows a bit of growth, but is still in the right ballpark..
        ArrayList<String> activeLocales = new ArrayList<String>(6);
        for (String locale : BuildConfig.COMPRESSED_LOCALES) {
            if (LocalizationUtils.chromiumLocaleMatchesLanguage(locale, uiLanguage)) {
                activeLocales.add(locale);
            }
        }
        if (activeLocales.isEmpty()) {
            assert BuildConfig.COMPRESSED_LOCALES.length > 0;
            assert Arrays.asList(BuildConfig.COMPRESSED_LOCALES).contains(FALLBACK_LOCALE);
            activeLocales.add(FALLBACK_LOCALE);
        }

        // * For bundles, locale pak files are always stored uncompressed
        //   either under base.apk!/assets/fallback-locales/<locale>.pak or
        //   base-<lang>.apk!/assets/locales#lang_<lang>/<locale>.pak. They
        //   never need to be extracted.
        //
        // * For regular APKs, the locale pak files are stored under:
        //      base.apk!/assets/locales/<locale>.pak
        //
        //   where <locale> is a Chromium-specific locale name.
        //
        AssetManager assetManager = ContextUtils.getApplicationAssets();
        if (!assetPathHasFile(
                    assetManager, COMPRESSED_LOCALES_DIR, activeLocales.get(0) + ".pak")) {
            if (DEBUG) {
                Log.i(TAG, "No locale pak files to extract, assuming app bundle.");
            }
            return new String[] {};
        }

        // This is a regular APK, and all pak files are available.
        // Return the list of locale pak file paths corresponding to the current language.
        String[] localePakFiles = new String[activeLocales.size()];
        for (int n = 0; n < activeLocales.size(); ++n) {
            localePakFiles[n] = COMPRESSED_LOCALES_DIR + '/' + activeLocales.get(n) + ".pak";
        }
        if (DEBUG) {
            Log.i(TAG, "UI Language '%s' will use %s.", uiLanguage,
                    Arrays.toString(localePakFiles));
        }

        return localePakFiles;
    }

    /**
     * Check that an AssetManager instance has a specific asset file.
     *
     * @param assetManager The application's AssetManager instance.
     * @param assetPath Asset directory path (e.g. "assets/locales").
     * @param assetFile Asset file name inside assetPath.
     * @return true iff the asset file is available.
     */
    private static boolean assetPathHasFile(
            AssetManager assetManager, String assetPath, String assetFile) {
        String assetFilePath = assetPath + '/' + assetFile;
        try {
            InputStream input = assetManager.open(assetFilePath);
            input.close();
            if (DEBUG) {
                Log.i(TAG, "Found asset file: " + assetFilePath);
            }
            return true;
        } catch (IOException e) {
            if (DEBUG) {
                Log.i(TAG, "No asset file at: " + assetFilePath);
            }
            return false;
        }
    }

    /**
     * Synchronously wait for the resource extraction to be completed.
     * <p>
     * This method is bad and you should feel bad for using it.
     *
     * @see #addCompletionCallback(Runnable)
     */
    public void waitForCompletion() {
        if (mExtractTask == null || shouldSkipPakExtraction()) {
            return;
        }

        try {
            mExtractTask.await();
        } catch (Exception e) {
            assert false;
        }
    }

    /**
     * Sets the traits to use for the reply task.
     */
    public void setResultTraits(TaskTraits traits) {
        mResultTaskTraits = traits;
    }

    /**
     * Adds a callback to be notified upon the completion of resource extraction.
     * <p>
     * If the resource task has already completed, the callback will be posted to the UI message
     * queue.  Otherwise, it will be executed after all the resources have been extracted.
     * <p>
     * This must be called on the UI thread.  The callback will also always be executed on
     * the UI thread.
     *
     * @param callback The callback to be enqueued.
     */
    public void addCompletionCallback(Runnable callback) {
        ThreadUtils.assertOnUiThread();

        if (shouldSkipPakExtraction()) {
            PostTask.postTask(mResultTaskTraits, callback);
            return;
        }

        assert mExtractTask != null;
        if (mExtractTask.isDone()) {
            PostTask.postTask(mResultTaskTraits, callback);
        } else {
            mExtractTask.mCompletionCallbacks.add(callback);
        }
    }

    /**
     * This will extract the application pak resources in an
     * AsyncTask. Call waitForCompletion() at the point resources
     * are needed to block until the task completes.
     *
     * @param uiLanguage The language to extract.
     */
    public void startExtractingResources(String uiLanguage) {
        if (mExtractTask != null) {
            return;
        }

        // If a previous release extracted resources, and the current release does not,
        // deleteFiles() will not run and some files will be left. This currently
        // can happen for ContentShell, but not for Chrome proper, since we always extract
        // locale pak files.
        if (shouldSkipPakExtraction()) {
            return;
        }

        mExtractTask = new ExtractTask(uiLanguage);
        PostTask.postTask(TaskTraits.USER_BLOCKING, mExtractTask);
    }

    private File getAppDataDir() {
        return new File(PathUtils.getDataDirectory());
    }

    private File getOutputDir() {
        return new File(getAppDataDir(), "paks");
    }

    private static void deleteFile(File file) {
        if (file.exists() && !file.delete()) {
            Log.w(TAG, "Unable to remove %s", file.getName());
        }
    }

    private void deleteFiles(String[] existingFileNames) {
        // These used to be extracted, but no longer are, so just clean them up.
        deleteFile(new File(getAppDataDir(), ICU_DATA_FILENAME));
        deleteFile(new File(getAppDataDir(), V8_NATIVES_DATA_FILENAME));
        deleteFile(new File(getAppDataDir(), V8_SNAPSHOT_DATA_FILENAME));

        if (existingFileNames != null) {
            for (String fileName : existingFileNames) {
                deleteFile(new File(getOutputDir(), fileName));
            }
        }
    }

    /**
     * Pak extraction not necessarily required by the embedder.
     */
    private static boolean shouldSkipPakExtraction() {
        // Certain apks like ContentShell.apk don't have any compressed locale
        // assets however, so skip extraction entirely for them.
        return BuildConfig.COMPRESSED_LOCALES.length == 0;
    }
}

#!/usr/bin/env python
#
# Copyright 2018 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Create an Android application bundle from one or more bundle modules."""

import argparse
import itertools
import json
import os
import shutil
import sys
import tempfile
import zipfile

# NOTE: Keep this consistent with the _create_app_bundle_py_imports definition
#       in build/config/android/rules.py
from util import build_utils
from util import resource_utils

import bundletool

# Location of language-based assets in bundle modules.
_LOCALES_SUBDIR = 'assets/locales/'

# The fallback locale should always have its .pak file included in
# the base apk, i.e. not use language-based asset targetting. This ensures
# that Chrome won't crash on startup if its bundle is installed on a device
# with an unsupported system locale (e.g. fur-rIT).
_FALLBACK_LOCALE = 'en-US'

# List of split dimensions recognized by this tool.
_ALL_SPLIT_DIMENSIONS = [ 'ABI', 'SCREEN_DENSITY', 'LANGUAGE' ]

# Due to historical reasons, certain languages identified by Chromium with a
# 3-letters ISO 639-2 code, are mapped to a nearly equivalent 2-letters
# ISO 639-1 code instead (due to the fact that older Android releases only
# supported the latter when matching resources).
#
# the same conversion as for Java resources.
_SHORTEN_LANGUAGE_CODE_MAP = {
  'fil': 'tl',  # Filipino to Tagalog.
}

# A list of extensions corresponding to files that should never be compressed
# in the bundle. This used to be handled by bundletool automatically until
# release 0.8.0, which required that this be passed to the BundleConfig
# file instead.
#
# This is the original list, which was taken from aapt2, with 'webp' added to
# it (which curiously was missing from the list).
_UNCOMPRESSED_FILE_EXTS = [
    '3g2', '3gp', '3gpp', '3gpp2', 'aac', 'amr', 'awb', 'git', 'imy', 'jet',
    'jpeg', 'jpg', 'm4a', 'm4v', 'mid', 'midi', 'mkv', 'mp2', 'mp3', 'mp4',
    'mpeg', 'mpg', 'ogg', 'png', 'rtttl', 'smf', 'wav', 'webm', 'webp', 'wmv',
    'xmf'
]


def _ParseArgs(args):
  parser = argparse.ArgumentParser()
  parser.add_argument('--out-bundle', required=True,
                      help='Output bundle zip archive.')
  parser.add_argument('--module-zips', required=True,
                      help='GN-list of module zip archives.')
  parser.add_argument(
      '--rtxt-in-paths', action='append', help='GN-list of module R.txt files.')
  parser.add_argument(
      '--rtxt-out-path', help='Path to combined R.txt file for bundle.')
  parser.add_argument('--uncompressed-assets', action='append',
                      help='GN-list of uncompressed assets.')
  parser.add_argument(
      '--compress-shared-libraries',
      action='store_true',
      help='Whether to store native libraries compressed.')
  parser.add_argument('--split-dimensions',
                      help="GN-list of split dimensions to support.")
  parser.add_argument(
      '--base-module-rtxt-path',
      help='Optional path to the base module\'s R.txt file, only used with '
      'language split dimension.')
  parser.add_argument(
      '--base-whitelist-rtxt-path',
      help='Optional path to an R.txt file, string resources '
      'listed there _and_ in --base-module-rtxt-path will '
      'be kept in the base bundle module, even if language'
      ' splitting is enabled.')

  parser.add_argument('--keystore-path', help='Keystore path')
  parser.add_argument('--keystore-password', help='Keystore password')
  parser.add_argument('--key-name', help='Keystore key name')

  options = parser.parse_args(args)
  options.module_zips = build_utils.ParseGnList(options.module_zips)
  options.rtxt_in_paths = build_utils.ExpandFileArgs(options.rtxt_in_paths)

  if len(options.module_zips) == 0:
    raise Exception('The module zip list cannot be empty.')

  # Signing is optional, but all --keyXX parameters should be set.
  if options.keystore_path or options.keystore_password or options.key_name:
    if not options.keystore_path or not options.keystore_password or \
        not options.key_name:
      raise Exception('When signing the bundle, use --keystore-path, '
                      '--keystore-password and --key-name.')

  # Merge all uncompressed assets into a set.
  uncompressed_list = []
  if options.uncompressed_assets:
    for l in options.uncompressed_assets:
      for entry in build_utils.ParseGnList(l):
        # Each entry has the following format: 'zipPath' or 'srcPath:zipPath'
        pos = entry.find(':')
        if pos >= 0:
          uncompressed_list.append(entry[pos + 1:])
        else:
          uncompressed_list.append(entry)

  options.uncompressed_assets = set(uncompressed_list)

  # Check that all split dimensions are valid
  if options.split_dimensions:
    options.split_dimensions = build_utils.ParseGnList(options.split_dimensions)
    for dim in options.split_dimensions:
      if dim.upper() not in _ALL_SPLIT_DIMENSIONS:
        parser.error('Invalid split dimension "%s" (expected one of: %s)' % (
            dim, ', '.join(x.lower() for x in _ALL_SPLIT_DIMENSIONS)))

  # As a special case, --base-whitelist-rtxt-path can be empty to indicate
  # that the module doesn't need such a whitelist. That's because it is easier
  # to check this condition here than through GN rules :-(
  if options.base_whitelist_rtxt_path == '':
    options.base_module_rtxt_path = None

  # Check --base-module-rtxt-path and --base-whitelist-rtxt-path usage.
  if options.base_module_rtxt_path:
    if not options.base_whitelist_rtxt_path:
      parser.error(
          '--base-module-rtxt-path requires --base-whitelist-rtxt-path')
    if 'language' not in options.split_dimensions:
      parser.error('--base-module-rtxt-path is only valid with '
                   'language-based splits.')

  return options


def _MakeSplitDimension(value, enabled):
  """Return dict modelling a BundleConfig splitDimension entry."""
  return {'value': value, 'negate': not enabled}


def _GenerateBundleConfigJson(uncompressed_assets, compress_shared_libraries,
                              split_dimensions, base_master_resource_ids):
  """Generate a dictionary that can be written to a JSON BuildConfig.

  Args:
    uncompressed_assets: A list or set of file paths under assets/ that always
      be stored uncompressed.
    compress_shared_libraries: Boolean, whether to compress native libs.
    split_dimensions: list of split dimensions.
    base_master_resource_ids: Optional list of 32-bit resource IDs to keep
      inside the base module, even when split dimensions are enabled.
  Returns:
    A dictionary that can be written as a json file.
  """
  # Compute splitsConfig list. Each item is a dictionary that can have
  # the following keys:
  #    'value': One of ['LANGUAGE', 'DENSITY', 'ABI']
  #    'negate': Boolean, True to indicate that the bundle should *not* be
  #              split (unused at the moment by this script).

  split_dimensions = [ _MakeSplitDimension(dim, dim in split_dimensions)
                       for dim in _ALL_SPLIT_DIMENSIONS ]

  # Native libraries loaded by the crazy linker.
  # Whether other .so files are compressed is controlled by
  # "uncompressNativeLibraries".
  uncompressed_globs = ['lib/*/crazy.*']
  # Locale-specific pak files stored in bundle splits need not be compressed.
  uncompressed_globs.extend(
      ['assets/locales#lang_*/*.pak', 'assets/fallback-locales/*.pak'])
  uncompressed_globs.extend('assets/' + x for x in uncompressed_assets)
  # NOTE: Use '**' instead of '*' to work through directories!
  uncompressed_globs.extend('**.' + ext for ext in _UNCOMPRESSED_FILE_EXTS)

  data = {
      'optimizations': {
          'splitsConfig': {
              'splitDimension': split_dimensions,
          },
          'uncompressNativeLibraries': {
              'enabled': not compress_shared_libraries,
          },
      },
      'compression': {
          'uncompressedGlob': sorted(uncompressed_globs),
      },
  }

  if base_master_resource_ids:
    data['master_resources'] = {
        'resource_ids': list(base_master_resource_ids),
    }

  return json.dumps(data, indent=2)


def _RewriteLanguageAssetPath(src_path):
  """Rewrite the destination path of a locale asset for language-based splits.

  Should only be used when generating bundles with language-based splits.
  This will rewrite paths that look like locales/<locale>.pak into
  locales#<language>/<locale>.pak, where <language> is the language code
  from the locale.

  Returns new path.
  """
  if not src_path.startswith(_LOCALES_SUBDIR) or not src_path.endswith('.pak'):
    return [src_path]

  locale = src_path[len(_LOCALES_SUBDIR):-4]
  android_locale = resource_utils.ToAndroidLocaleName(locale)

  # The locale format is <lang>-<region> or <lang>. Extract the language.
  pos = android_locale.find('-')
  if pos >= 0:
    android_language = android_locale[:pos]
  else:
    android_language = android_locale

  if locale == _FALLBACK_LOCALE:
    # Fallback locale .pak files must be placed in a different directory
    # to ensure they are always stored in the base module.
    result_path = 'assets/fallback-locales/%s.pak' % locale
  else:
    # Other language .pak files go into a language-specific asset directory
    # that bundletool will store in separate split APKs.
    result_path = 'assets/locales#lang_%s/%s.pak' % (android_language, locale)

  return result_path


def _SplitModuleForAssetTargeting(src_module_zip, tmp_dir, split_dimensions):
  """Splits assets in a module if needed.

  Args:
    src_module_zip: input zip module path.
    tmp_dir: Path to temporary directory, where the new output module might
      be written to.
    split_dimensions: list of split dimensions.

  Returns:
    If the module doesn't need asset targeting, doesn't do anything and
    returns src_module_zip. Otherwise, create a new module zip archive under
    tmp_dir with the same file name, but which contains assets paths targeting
    the proper dimensions.
  """
  split_language = 'LANGUAGE' in split_dimensions
  if not split_language:
    # Nothing to target, so return original module path.
    return src_module_zip

  with zipfile.ZipFile(src_module_zip, 'r') as src_zip:
    language_files = [
      f for f in src_zip.namelist() if f.startswith(_LOCALES_SUBDIR)]

    if not language_files:
      # Not language-based assets to split in this module.
      return src_module_zip

    tmp_zip = os.path.join(tmp_dir, os.path.basename(src_module_zip))
    with zipfile.ZipFile(tmp_zip, 'w') as dst_zip:
      for info in src_zip.infolist():
        src_path = info.filename
        is_compressed = info.compress_type != zipfile.ZIP_STORED

        dst_path = src_path
        if src_path in language_files:
          dst_path = _RewriteLanguageAssetPath(src_path)

        build_utils.AddToZipHermetic(
            dst_zip,
            dst_path,
            data=src_zip.read(src_path),
            compress=is_compressed)

    return tmp_zip


def _GenerateBaseResourcesWhitelist(base_module_rtxt_path,
                                    base_whitelist_rtxt_path):
  """Generate a whitelist of base master resource ids.

  Args:
    base_module_rtxt_path: Path to base module R.txt file.
    base_whitelist_rtxt_path: Path to base whitelist R.txt file.
  Returns:
    list of resource ids.
  """
  ids_map = resource_utils.GenerateStringResourcesWhitelist(
      base_module_rtxt_path, base_whitelist_rtxt_path)
  return ids_map.keys()


def main(args):
  args = build_utils.ExpandFileArgs(args)
  options = _ParseArgs(args)

  split_dimensions = []
  if options.split_dimensions:
    split_dimensions = [x.upper() for x in options.split_dimensions]


  with build_utils.TempDir() as tmp_dir:
    module_zips = [
        _SplitModuleForAssetTargeting(module, tmp_dir, split_dimensions) \
        for module in options.module_zips]

    base_master_resource_ids = None
    if options.base_module_rtxt_path:
      base_master_resource_ids = _GenerateBaseResourcesWhitelist(
          options.base_module_rtxt_path, options.base_whitelist_rtxt_path)

    bundle_config = _GenerateBundleConfigJson(
        options.uncompressed_assets, options.compress_shared_libraries,
        split_dimensions, base_master_resource_ids)

    tmp_bundle = os.path.join(tmp_dir, 'tmp_bundle')

    tmp_unsigned_bundle = tmp_bundle
    if options.keystore_path:
      tmp_unsigned_bundle = tmp_bundle + '.unsigned'

    # Important: bundletool requires that the bundle config file is
    # named with a .pb.json extension.
    tmp_bundle_config = tmp_bundle + '.BundleConfig.pb.json'

    with open(tmp_bundle_config, 'w') as f:
      f.write(bundle_config)

    cmd_args = ['java', '-jar', bundletool.BUNDLETOOL_JAR_PATH, 'build-bundle']
    cmd_args += ['--modules=%s' % ','.join(module_zips)]
    cmd_args += ['--output=%s' % tmp_unsigned_bundle]
    cmd_args += ['--config=%s' % tmp_bundle_config]

    build_utils.CheckOutput(cmd_args, print_stdout=True, print_stderr=True)

    if options.keystore_path:
      # NOTE: As stated by the public documentation, apksigner cannot be used
      # to sign the bundle (because it rejects anything that isn't an APK).
      # The signature and digest algorithm selection come from the internal
      # App Bundle documentation. There is no corresponding public doc :-(
      signing_cmd_args = [
          'jarsigner', '-sigalg', 'SHA256withRSA', '-digestalg', 'SHA-256',
          '-keystore', 'file:' + options.keystore_path,
          '-storepass' , options.keystore_password,
          '-signedjar', tmp_bundle,
          tmp_unsigned_bundle,
          options.key_name,
      ]
      build_utils.CheckOutput(signing_cmd_args, print_stderr=True)

    shutil.move(tmp_bundle, options.out_bundle)

  if options.rtxt_out_path:
    with open(options.rtxt_out_path, 'w') as rtxt_out:
      for rtxt_in_path in options.rtxt_in_paths:
        with open(rtxt_in_path, 'r') as rtxt_in:
          rtxt_out.write('-- Contents of {}\n'.format(
              os.path.basename(rtxt_in_path)))
          rtxt_out.write(rtxt_in.read())


if __name__ == '__main__':
  main(sys.argv[1:])

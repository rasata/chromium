#!/usr/bin/python2
#
# Copyright 2016 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Archive corpus file into zip and generate .d depfile.

Invoked by GN from fuzzer_test.gni.
"""

from __future__ import print_function
import argparse
import os
import sys
import warnings
import zipfile

SEED_CORPUS_LIMIT_MB = 100


def main():
  parser = argparse.ArgumentParser(description="Generate fuzzer config.")
  parser.add_argument('corpus_directories', metavar='corpus_dir', type=str,
                      nargs='+')
  parser.add_argument('--output', metavar='output_archive_name.zip',
                      required=True)
  parser.add_argument('--dry_run', default=False, action='store_true',
                      help="Don't actually create the output archive.")

  args = parser.parse_args()
  corpus_files = []
  seed_corpus_path = args.output

  for directory in args.corpus_directories:
    if not os.path.exists(directory):
      raise Exception('The given seed_corpus directory (%s) does not exist.' %
                      directory)
    for (dirpath, _, filenames) in os.walk(directory):
      for filename in filenames:
        full_filename = os.path.join(dirpath, filename)
        corpus_files.append(full_filename)

  if args.dry_run:
    return

  with zipfile.ZipFile(seed_corpus_path, 'w') as z:
    # Turn warnings into errors to interrupt the build: crbug.com/653920.
    with warnings.catch_warnings():
      warnings.simplefilter("error")
      for i, corpus_file in enumerate(corpus_files):
        # To avoid duplication of filenames inside the archive, use numbers.
        arcname = '%016d' % i
        z.write(corpus_file, arcname)

  if os.path.getsize(seed_corpus_path) > SEED_CORPUS_LIMIT_MB * 1024 * 1024:
    print('Seed corpus %s exceeds maximum allowed size (%d MB).' %
          (seed_corpus_path, SEED_CORPUS_LIMIT_MB))
    sys.exit(-1)

if __name__ == '__main__':
  main()

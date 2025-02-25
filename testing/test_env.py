#!/usr/bin/env python
# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Sets environment variables needed to run a chromium unit test."""

import io
import os
import signal
import stat
import subprocess
import sys
import time

# This is hardcoded to be src/ relative to this script.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CHROME_SANDBOX_ENV = 'CHROME_DEVEL_SANDBOX'
CHROME_SANDBOX_PATH = '/opt/chromium/chrome_sandbox'


def get_sandbox_env(env):
  """Returns the environment flags needed for the SUID sandbox to work."""
  extra_env = {}
  chrome_sandbox_path = env.get(CHROME_SANDBOX_ENV, CHROME_SANDBOX_PATH)
  # The above would silently disable the SUID sandbox if the env value were
  # an empty string. We don't want to allow that. http://crbug.com/245376
  # TODO(jln): Remove this check once it's no longer possible to disable the
  # sandbox that way.
  if not chrome_sandbox_path:
    chrome_sandbox_path = CHROME_SANDBOX_PATH
  extra_env[CHROME_SANDBOX_ENV] = chrome_sandbox_path

  return extra_env


def trim_cmd(cmd):
  """Removes internal flags from cmd since they're just used to communicate from
  the host machine to this script running on the swarm slaves."""
  sanitizers = ['asan', 'lsan', 'msan', 'tsan']
  internal_flags = frozenset('--%s=%d' % (name, value)
                             for name in sanitizers
                             for value in [0, 1])
  return [i for i in cmd if i not in internal_flags]


def fix_python_path(cmd):
  """Returns the fixed command line to call the right python executable."""
  out = cmd[:]
  if out[0] == 'python':
    out[0] = sys.executable
  elif out[0].endswith('.py'):
    out.insert(0, sys.executable)
  return out


def get_sanitizer_env(cmd, asan, lsan, msan, tsan, cfi_diag):
  """Returns the envirnoment flags needed for sanitizer tools."""

  extra_env = {}

  # Instruct GTK to use malloc while running sanitizer-instrumented tests.
  extra_env['G_SLICE'] = 'always-malloc'

  extra_env['NSS_DISABLE_ARENA_FREE_LIST'] = '1'
  extra_env['NSS_DISABLE_UNLOAD'] = '1'

  # TODO(glider): remove the symbolizer path once
  # https://code.google.com/p/address-sanitizer/issues/detail?id=134 is fixed.
  symbolizer_path = os.path.join(ROOT_DIR,
      'third_party', 'llvm-build', 'Release+Asserts', 'bin', 'llvm-symbolizer')

  if lsan or tsan:
    # LSan is not sandbox-compatible, so we can use online symbolization. In
    # fact, it needs symbolization to be able to apply suppressions.
    symbolization_options = ['symbolize=1',
                             'external_symbolizer_path=%s' % symbolizer_path]
  elif (asan or msan or cfi_diag) and sys.platform not in ['win32', 'cygwin']:
    # ASan uses a script for offline symbolization, except on Windows.
    # Important note: when running ASan with leak detection enabled, we must use
    # the LSan symbolization options above.
    symbolization_options = ['symbolize=0']
    # Set the path to llvm-symbolizer to be used by asan_symbolize.py
    extra_env['LLVM_SYMBOLIZER_PATH'] = symbolizer_path
  else:
    symbolization_options = []

  # Leverage sanitizer to print stack trace on abort (e.g. assertion failure).
  symbolization_options.append('handle_abort=1')

  if asan:
    asan_options = symbolization_options[:]
    if lsan:
      asan_options.append('detect_leaks=1')

    if asan_options:
      extra_env['ASAN_OPTIONS'] = ' '.join(asan_options)

    if sys.platform == 'darwin':
      isolate_output_dir = os.path.abspath(os.path.dirname(cmd[0]))
      # This is needed because the test binary has @executable_path embedded in
      # it that the OS tries to resolve to the cache directory and not the
      # mapped directory.
      extra_env['DYLD_LIBRARY_PATH'] = str(isolate_output_dir)

  if lsan:
    if asan or msan:
      lsan_options = []
    else:
      lsan_options = symbolization_options[:]
    if sys.platform == 'linux2':
      # Use the debug version of libstdc++ under LSan. If we don't, there will
      # be a lot of incomplete stack traces in the reports.
      extra_env['LD_LIBRARY_PATH'] = '/usr/lib/x86_64-linux-gnu/debug:'

    extra_env['LSAN_OPTIONS'] = ' '.join(lsan_options)

  if msan:
    msan_options = symbolization_options[:]
    if lsan:
      msan_options.append('detect_leaks=1')
    extra_env['MSAN_OPTIONS'] = ' '.join(msan_options)

  if tsan:
    tsan_options = symbolization_options[:]
    extra_env['TSAN_OPTIONS'] = ' '.join(tsan_options)

  # CFI uses the UBSan runtime to provide diagnostics.
  if cfi_diag:
    ubsan_options = symbolization_options[:] + ['print_stacktrace=1']
    extra_env['UBSAN_OPTIONS'] = ' '.join(ubsan_options)

  return extra_env


def get_sanitizer_symbolize_command(json_path=None, executable_path=None):
  """Construct the command to invoke offline symbolization script."""
  script_path = os.path.join(
      ROOT_DIR, 'tools', 'valgrind', 'asan', 'asan_symbolize.py')
  cmd = [sys.executable, script_path]
  if json_path is not None:
    cmd.append('--test-summary-json-file=%s' % json_path)
  if executable_path is not None:
    cmd.append('--executable-path=%s' % executable_path)
  return cmd


def get_json_path(cmd):
  """Extract the JSON test summary path from a command line."""
  json_path_flag = '--test-launcher-summary-output='
  for arg in cmd:
    if arg.startswith(json_path_flag):
      return arg.split(json_path_flag).pop()
  return None


def symbolize_snippets_in_json(cmd, env):
  """Symbolize output snippets inside the JSON test summary."""
  json_path = get_json_path(cmd)
  if json_path is None:
    return

  try:
    symbolize_command = get_sanitizer_symbolize_command(
        json_path=json_path, executable_path=cmd[0])
    p = subprocess.Popen(symbolize_command, stderr=subprocess.PIPE, env=env)
    (_, stderr) = p.communicate()
  except OSError as e:
    print >> sys.stderr, 'Exception while symbolizing snippets: %s' % e
    raise

  if p.returncode != 0:
    print >> sys.stderr, "Error: failed to symbolize snippets in JSON:\n"
    print >> sys.stderr, stderr
    raise subprocess.CalledProcessError(p.returncode, symbolize_command)


def run_command_with_output(argv, stdoutfile, env=None, cwd=None):
  """Run command and stream its stdout/stderr to the console & |stdoutfile|.

  Also forward_signals to obey
  https://chromium.googlesource.com/infra/luci/luci-py/+/master/appengine/swarming/doc/Bot.md#graceful-termination_aka-the-sigterm-and-sigkill-dance

  Returns:
    integer returncode of the subprocess.
  """
  print('Running %r in %r (env: %r)' % (argv, cwd, env))
  assert stdoutfile
  with io.open(stdoutfile, 'wb') as writer, \
      io.open(stdoutfile, 'rb', 1) as reader:
    process = subprocess.Popen(argv, env=env, cwd=cwd, stdout=writer,
        stderr=subprocess.STDOUT)
    forward_signals([process])
    while process.poll() is None:
      sys.stdout.write(reader.read())
      # This sleep is needed for signal propagation. See the
      # wait_with_signals() docstring.
      time.sleep(0.1)
    # Read the remaining.
    sys.stdout.write(reader.read())
    print('Command %r returned exit code %d' % (argv, process.returncode))
    return process.returncode


def run_command(argv, env=None, cwd=None, log=True):
  """Run command and stream its stdout/stderr both to stdout.

  Also forward_signals to obey
  https://chromium.googlesource.com/infra/luci/luci-py/+/master/appengine/swarming/doc/Bot.md#graceful-termination_aka-the-sigterm-and-sigkill-dance

  Returns:
    integer returncode of the subprocess.
  """
  if log:
    print('Running %r in %r (env: %r)' % (argv, cwd, env))
  process = subprocess.Popen(argv, env=env, cwd=cwd, stderr=subprocess.STDOUT)
  forward_signals([process])
  return wait_with_signals(process)


def wait_with_signals(process):
  """A version of process.wait() that works cross-platform.

  This version properly surfaces the SIGBREAK signal.

  From reading the subprocess.py source code, it seems we need to explicitly
  call time.sleep(). The reason is that subprocess.Popen.wait() on Windows
  directly calls WaitForSingleObject(), but only time.sleep() properly surface
  the SIGBREAK signal.

  Refs:
  https://github.com/python/cpython/blob/v2.7.15/Lib/subprocess.py#L692
  https://github.com/python/cpython/blob/v2.7.15/Modules/timemodule.c#L1084

  Returns:
    returncode of the process.
  """
  while process.poll() is None:
    time.sleep(0.1)
  return process.returncode


def forward_signals(procs):
  """Forwards unix's SIGTERM or win's CTRL_BREAK_EVENT to the given processes.

  This plays nicely with swarming's timeout handling. See also
  https://chromium.googlesource.com/infra/luci/luci-py/+/master/appengine/swarming/doc/Bot.md#graceful-termination_aka-the-sigterm-and-sigkill-dance

  Args:
      procs: A list of subprocess.Popen objects representing child processes.
  """
  assert all(isinstance(p, subprocess.Popen) for p in procs)
  def _sig_handler(sig, _):
    for p in procs:
      # SIGBREAK is defined only for win32.
      if sys.platform == 'win32' and sig == signal.SIGBREAK:
        p.send_signal(signal.CTRL_BREAK_EVENT)
      else:
        p.send_signal(sig)
  if sys.platform == 'win32':
    signal.signal(signal.SIGBREAK, _sig_handler)
  else:
    signal.signal(signal.SIGTERM, _sig_handler)


def run_executable(cmd, env, stdoutfile=None):
  """Runs an executable with:
    - CHROME_HEADLESS set to indicate that the test is running on a
      bot and shouldn't do anything interactive like show modal dialogs.
    - environment variable CR_SOURCE_ROOT set to the root directory.
    - environment variable LANGUAGE to en_US.UTF-8.
    - environment variable CHROME_DEVEL_SANDBOX set
    - Reuses sys.executable automatically.
  """
  extra_env = {
      # Set to indicate that the executable is running non-interactively on
      # a bot.
      'CHROME_HEADLESS': '1',

       # Many tests assume a English interface...
      'LANG': 'en_US.UTF-8',
  }

  # Used by base/base_paths_linux.cc as an override. Just make sure the default
  # logic is used.
  env.pop('CR_SOURCE_ROOT', None)
  extra_env.update(get_sandbox_env(env))

  # Copy logic from  tools/build/scripts/slave/runtest.py.
  asan = '--asan=1' in cmd
  lsan = '--lsan=1' in cmd
  msan = '--msan=1' in cmd
  tsan = '--tsan=1' in cmd
  cfi_diag = '--cfi-diag=1' in cmd
  if stdoutfile or sys.platform in ['win32', 'cygwin']:
    # Symbolization works in-process on Windows even when sandboxed.
    use_symbolization_script = False
  else:
    # LSan doesn't support sandboxing yet, so we use the in-process symbolizer.
    # Note that ASan and MSan can work together with LSan.
    use_symbolization_script = (asan or msan or cfi_diag) and not lsan

  if asan or lsan or msan or tsan or cfi_diag:
    extra_env.update(get_sanitizer_env(cmd, asan, lsan, msan, tsan, cfi_diag))

  if lsan or tsan:
    # LSan and TSan are not sandbox-friendly.
    cmd.append('--no-sandbox')

  cmd = trim_cmd(cmd)

  # Ensure paths are correctly separated on windows.
  cmd[0] = cmd[0].replace('/', os.path.sep)
  cmd = fix_python_path(cmd)

  # We also want to print the GTEST env vars that were set by the caller,
  # because you need them to reproduce the task properly.
  env_to_print = extra_env.copy()
  for env_var_name in ('GTEST_SHARD_INDEX', 'GTEST_TOTAL_SHARDS'):
      if env_var_name in env:
          env_to_print[env_var_name] = env[env_var_name]

  print('Additional test environment:\n%s\n'
        'Command: %s\n' % (
        '\n'.join('    %s=%s' %
            (k, v) for k, v in sorted(env_to_print.iteritems())),
        ' '.join(cmd)))
  sys.stdout.flush()
  env.update(extra_env or {})
  try:
    if stdoutfile:
      # Write to stdoutfile and poll to produce terminal output.
      return run_command_with_output(cmd, env=env, stdoutfile=stdoutfile)
    elif use_symbolization_script:
      # See above comment regarding offline symbolization.
      # Need to pipe to the symbolizer script.
      p1 = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE,
                            stderr=sys.stdout)
      p2 = subprocess.Popen(
          get_sanitizer_symbolize_command(executable_path=cmd[0]),
          env=env, stdin=p1.stdout)
      p1.stdout.close()  # Allow p1 to receive a SIGPIPE if p2 exits.
      forward_signals([p1, p2])
      wait_with_signals(p1)
      wait_with_signals(p2)
      # Also feed the out-of-band JSON output to the symbolizer script.
      symbolize_snippets_in_json(cmd, env)
      return p1.returncode
    else:
      return run_command(cmd, env=env, log=False)
  except OSError:
    print >> sys.stderr, 'Failed to start %s' % cmd
    raise


def main():
  return run_executable(sys.argv[1:], os.environ.copy())


if __name__ == '__main__':
  sys.exit(main())

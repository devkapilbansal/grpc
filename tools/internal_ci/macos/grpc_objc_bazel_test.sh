#!/usr/bin/env bash
# Copyright 2022 The gRPC Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -ex

# avoid slow finalization after the script has exited.
source $(dirname $0)/../../../tools/internal_ci/helper_scripts/move_src_tree_and_respawn_itself_rc

# change to grpc repo root
cd $(dirname $0)/../../..

source tools/internal_ci/helper_scripts/prepare_build_macos_rc

# make sure bazel is available
tools/bazel version

# for kokoro mac workers, exact image version is store in a well-known location on disk
KOKORO_IMAGE_VERSION="$(cat /VERSION)"

BAZEL_REMOTE_CACHE_ARGS=(
  # Enable uploading to remote cache. Requires the "roles/remotebuildexecution.actionCacheWriter" permission.
  --remote_upload_local_results=true
  # allow invalidating the old cache by setting to a new random key
  --remote_default_exec_properties="grpc_cache_silo_key1=83d8e488-1ca9-40fd-929e-d37d13529c99"
  # make sure we only get cache hits from binaries built on exact same macos image
  --remote_default_exec_properties="grpc_cache_silo_key2=${KOKORO_IMAGE_VERSION}"
)

EXAMPLE_TARGETS=(
  # TODO(jtattermusch): ideally we'd say "//src/objective-c/examples/..." but not all the targets currently build
  //src/objective-c/examples:Sample
  //src/objective-c/examples:tvOS-sample
)

TEST_TARGETS=(
  # TODO(jtattermusch): ideally we'd say "//src/objective-c/tests/..." but not all the targets currently build
  # TODO(jtattermusch): make //src/objective-c/tests:TvTests build reliably
  # TODO(jtattermusch): make //src/objective-c/tests:MacTests build reliably
  //src/objective-c/tests:UnitTests
)

# === BEGIN SECTION: run interop_server on the background ====
# Before testing objC at all, build the interop server since many of the ObjC test rely on it.
# Use remote cache to build the interop_server binary as quickly as possible (interop_server
# is not what we want to test actually, we just use it as a backend for ObjC test).
# TODO(jtattermusch): can we make ObjC test not depend on running a local interop_server?
python3 tools/run_tests/python_utils/bazel_report_helper.py --report_path build_interop_server
build_interop_server/bazel_wrapper \
  --bazelrc=tools/remote_build/mac.bazelrc \
  build \
  --google_credentials="${KOKORO_GFILE_DIR}/GrpcTesting-d0eeee2db331.json" \
  "${BAZEL_REMOTE_CACHE_ARGS[@]}" \
  -- \
  //test/cpp/interop:interop_server

INTEROP_SERVER_BINARY=bazel-bin/test/cpp/interop/interop_server
# run the interop server on the background. The port numbers must match TestConfigs in BUILD.
# TODO(jtattermusch): can we make the ports configurable (but avoid breaking bazel build cache at the same time?)
"${INTEROP_SERVER_BINARY}" --port=5050 --max_send_message_size=8388608 &
"${INTEROP_SERVER_BINARY}" --port=5051 --max_send_message_size=8388608 --use_tls &
# make sure the interop_server processes we started on the background are killed upon exit.
trap 'echo "KILLING interop_server binaries running on the background"; kill -9 $(jobs -p)' EXIT
# === END SECTION: run interop_server on the background ====

# TODO(jtattermusch): set GRPC_VERBOSITY=debug when running tests on a simulator (how to do that?)

python3 tools/run_tests/python_utils/bazel_report_helper.py --report_path objc_bazel_tests

objc_bazel_tests/bazel_wrapper \
  --bazelrc=tools/remote_build/include/test_locally_with_resultstore_results.bazelrc \
  test \
  --google_credentials="${KOKORO_GFILE_DIR}/GrpcTesting-d0eeee2db331.json" \
  $BAZEL_FLAGS \
  -- \
  "${EXAMPLE_TARGETS[@]}" \
  "${TEST_TARGETS[@]}"

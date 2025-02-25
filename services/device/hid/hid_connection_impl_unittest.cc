// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/hid/hid_connection_impl.h"

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/memory/ref_counted_memory.h"
#include "build/build_config.h"
#include "mojo/public/cpp/bindings/binding.h"
#include "services/device/device_service_test_base.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace device {

namespace {

#if defined(OS_MACOSX)
const uint64_t kTestDeviceId = 123;
#else
const char* kTestDeviceId = "123";
#endif

// The report ID to use for reports sent to or received from the test device.
const uint8_t kTestReportId = 0x42;

// The max size of input and output reports for the test device. Feature reports
// are not used in this test.
const uint64_t kMaxReportSizeBytes = 10;

// A fake HidConnection implementation that allows the test to simulate an
// input report.
class FakeHidConnection : public HidConnection {
 public:
  FakeHidConnection(scoped_refptr<HidDeviceInfo> device)
      : HidConnection(device) {}

  // HidConnection implementation.
  void PlatformClose() override {}
  void PlatformWrite(scoped_refptr<base::RefCountedBytes> buffer,
                     WriteCallback callback) override {
    std::move(callback).Run(true);
  }
  void PlatformGetFeatureReport(uint8_t report_id,
                                ReadCallback callback) override {
    NOTIMPLEMENTED();
  }
  void PlatformSendFeatureReport(scoped_refptr<base::RefCountedBytes> buffer,
                                 WriteCallback callback) override {
    NOTIMPLEMENTED();
  }

  void SimulateInputReport(scoped_refptr<base::RefCountedBytes> buffer) {
    ProcessInputReport(buffer, buffer->size());
  }

 private:
  ~FakeHidConnection() override = default;

  DISALLOW_COPY_AND_ASSIGN(FakeHidConnection);
};

// A test implementation of HidConnectionClient that signals once an input
// report has been received. The contents of the input report are saved.
class TestHidConnectionClient : public mojom::HidConnectionClient {
 public:
  TestHidConnectionClient() : binding_(this) {}

  void Bind(mojom::HidConnectionClientRequest request) {
    binding_.Bind(std::move(request));
  }

  // mojom::HidConnectionClient implementation.
  void OnInputReport(uint8_t report_id,
                     const std::vector<uint8_t>& buffer) override {
    report_id_ = report_id;
    buffer_ = buffer;
    run_loop_.Quit();
  }

  void WaitForInputReport() { run_loop_.Run(); }

  uint8_t report_id() { return report_id_; }
  const std::vector<uint8_t>& buffer() { return buffer_; }

 private:
  base::RunLoop run_loop_;
  mojo::Binding<mojom::HidConnectionClient> binding_;
  uint8_t report_id_ = 0;
  std::vector<uint8_t> buffer_;

  DISALLOW_COPY_AND_ASSIGN(TestHidConnectionClient);
};

// A utility for capturing the state returned by mojom::HidConnection I/O
// callbacks.
class TestIoCallback {
 public:
  TestIoCallback() = default;
  ~TestIoCallback() = default;

  void SetReadResult(bool result,
                     uint8_t report_id,
                     const base::Optional<std::vector<uint8_t>>& buffer) {
    result_ = result;
    report_id_ = report_id;
    has_buffer_ = buffer.has_value();
    if (has_buffer_)
      buffer_ = *buffer;
    run_loop_.Quit();
  }

  void SetWriteResult(bool result) {
    result_ = result;
    run_loop_.Quit();
  }

  bool WaitForResult() {
    run_loop_.Run();
    return result_;
  }

  mojom::HidConnection::ReadCallback GetReadCallback() {
    return base::BindOnce(&TestIoCallback::SetReadResult,
                          base::Unretained(this));
  }

  mojom::HidConnection::WriteCallback GetWriteCallback() {
    return base::BindOnce(&TestIoCallback::SetWriteResult,
                          base::Unretained(this));
  }

  uint8_t report_id() { return report_id_; }
  bool has_buffer() { return has_buffer_; }
  const std::vector<uint8_t>& buffer() { return buffer_; }

 private:
  base::RunLoop run_loop_;
  bool result_ = false;
  uint8_t report_id_ = 0;
  bool has_buffer_ = false;
  std::vector<uint8_t> buffer_;
};

}  // namespace

class HidConnectionImplTest : public DeviceServiceTestBase {
 public:
  HidConnectionImplTest() = default;

 protected:
  void SetUp() override {
    DeviceServiceTestBase::SetUp();
    base::RunLoop().RunUntilIdle();
  }

  void CreateHidConnection(bool with_connection_client) {
    mojom::HidConnectionClientPtr hid_connection_client;
    if (with_connection_client) {
      connection_client_ = std::make_unique<TestHidConnectionClient>();
      connection_client_->Bind(mojo::MakeRequest(&hid_connection_client));
    }
    fake_connection_ = new FakeHidConnection(CreateTestDevice());
    hid_connection_impl_ = std::make_unique<HidConnectionImpl>(
        fake_connection_, std::move(hid_connection_client));
  }

  scoped_refptr<HidDeviceInfo> CreateTestDevice() {
    auto hid_collection_info = mojom::HidCollectionInfo::New();
    hid_collection_info->usage = mojom::HidUsageAndPage::New(0, 0);
    hid_collection_info->report_ids.push_back(kTestReportId);
    return base::MakeRefCounted<HidDeviceInfo>(
        kTestDeviceId, 0x1234, 0xabcd, "product name", "serial number",
        mojom::HidBusType::kHIDBusTypeUSB, std::move(hid_collection_info),
        kMaxReportSizeBytes, kMaxReportSizeBytes, 0);
  }

  std::unique_ptr<HidConnectionImpl> hid_connection_impl_;
  scoped_refptr<FakeHidConnection> fake_connection_;
  std::unique_ptr<TestHidConnectionClient> connection_client_;
};

TEST_F(HidConnectionImplTest, ReadWrite) {
  CreateHidConnection(false);
  // Buffer contents: [0x42 0x01 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09]
  const size_t kTestBufferSize = kMaxReportSizeBytes;
  std::vector<uint8_t> buffer_vec(kTestBufferSize);
  buffer_vec[0] = kTestReportId;
  for (size_t i = 1; i < kTestBufferSize; ++i)
    buffer_vec[i] = i;

  // Simulate an output report (host to device).
  TestIoCallback write_callback;
  hid_connection_impl_->Write(kTestReportId, buffer_vec,
                              write_callback.GetWriteCallback());
  ASSERT_TRUE(write_callback.WaitForResult());

  // Simulate an input report (device to host).
  auto buffer = base::MakeRefCounted<base::RefCountedBytes>(buffer_vec);
  ASSERT_EQ(buffer->size(), kTestBufferSize);
  fake_connection_->SimulateInputReport(buffer);

  // Simulate reading the input report.
  TestIoCallback read_callback;
  hid_connection_impl_->Read(read_callback.GetReadCallback());
  ASSERT_TRUE(read_callback.WaitForResult());
  EXPECT_EQ(read_callback.report_id(), kTestReportId);
  ASSERT_TRUE(read_callback.has_buffer());
  const auto& read_buffer = read_callback.buffer();
  ASSERT_EQ(read_buffer.size(), kTestBufferSize - 1);
  for (size_t i = 1; i < kTestBufferSize; ++i) {
    EXPECT_EQ(read_buffer[i - 1], buffer_vec[i])
        << "Mismatch at index " << i << ".";
  }
}

TEST_F(HidConnectionImplTest, ReadWriteWithConnectionClient) {
  CreateHidConnection(true);
  // Buffer contents: [0x42 0x01 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09]
  const size_t kTestBufferSize = kMaxReportSizeBytes;
  std::vector<uint8_t> buffer_vec(kTestBufferSize);
  buffer_vec[0] = kTestReportId;
  for (size_t i = 1; i < kTestBufferSize; ++i)
    buffer_vec[i] = i;

  // Simulate an output report (host to device).
  TestIoCallback write_callback;
  hid_connection_impl_->Write(kTestReportId, buffer_vec,
                              write_callback.GetWriteCallback());
  ASSERT_TRUE(write_callback.WaitForResult());

  // Simulate an input report (device to host).
  auto buffer = base::MakeRefCounted<base::RefCountedBytes>(buffer_vec);
  ASSERT_EQ(buffer->size(), kTestBufferSize);
  fake_connection_->SimulateInputReport(buffer);
  connection_client_->WaitForInputReport();

  // The connection client should have been notified.
  EXPECT_EQ(connection_client_->report_id(), kTestReportId);
  const std::vector<uint8_t>& in_buffer = connection_client_->buffer();
  ASSERT_EQ(in_buffer.size(), kTestBufferSize - 1);
  for (size_t i = 1; i < kTestBufferSize; ++i) {
    EXPECT_EQ(in_buffer[i - 1], buffer_vec[i])
        << "Mismatch at index " << i << ".";
  }
}

}  // namespace device

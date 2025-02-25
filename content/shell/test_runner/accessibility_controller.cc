// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/shell/test_runner/accessibility_controller.h"

#include <string>

#include "base/stl_util.h"
#include "content/shell/test_runner/web_view_test_proxy.h"
#include "gin/handle.h"
#include "gin/object_template_builder.h"
#include "gin/wrappable.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/blink/public/web/web_ax_context.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_frame.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_view.h"

namespace test_runner {

class AccessibilityControllerBindings
    : public gin::Wrappable<AccessibilityControllerBindings> {
 public:
  static gin::WrapperInfo kWrapperInfo;

  static void Install(base::WeakPtr<AccessibilityController> controller,
                      blink::WebLocalFrame* frame);

 private:
  explicit AccessibilityControllerBindings(
      base::WeakPtr<AccessibilityController> controller);
  ~AccessibilityControllerBindings() override;

  // gin::Wrappable:
  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override;

  void LogAccessibilityEvents();
  void SetNotificationListener(v8::Local<v8::Function> callback);
  void UnsetNotificationListener();
  v8::Local<v8::Object> FocusedElement();
  v8::Local<v8::Object> RootElement();
  v8::Local<v8::Object> AccessibleElementById(const std::string& id);

  base::WeakPtr<AccessibilityController> controller_;

  DISALLOW_COPY_AND_ASSIGN(AccessibilityControllerBindings);
};

gin::WrapperInfo AccessibilityControllerBindings::kWrapperInfo = {
    gin::kEmbedderNativeGin};

// static
void AccessibilityControllerBindings::Install(
    base::WeakPtr<AccessibilityController> controller,
    blink::WebLocalFrame* frame) {
  v8::Isolate* isolate = blink::MainThreadIsolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = frame->MainWorldScriptContext();
  if (context.IsEmpty())
    return;

  v8::Context::Scope context_scope(context);

  gin::Handle<AccessibilityControllerBindings> bindings = gin::CreateHandle(
      isolate, new AccessibilityControllerBindings(controller));
  if (bindings.IsEmpty())
    return;
  v8::Local<v8::Object> global = context->Global();
  global
      ->Set(context, gin::StringToV8(isolate, "accessibilityController"),
            bindings.ToV8())
      .Check();
}

AccessibilityControllerBindings::AccessibilityControllerBindings(
    base::WeakPtr<AccessibilityController> controller)
    : controller_(controller) {}

AccessibilityControllerBindings::~AccessibilityControllerBindings() {}

gin::ObjectTemplateBuilder
AccessibilityControllerBindings::GetObjectTemplateBuilder(
    v8::Isolate* isolate) {
  return gin::Wrappable<
             AccessibilityControllerBindings>::GetObjectTemplateBuilder(isolate)
      .SetMethod("logAccessibilityEvents",
                 &AccessibilityControllerBindings::LogAccessibilityEvents)
      .SetMethod("setNotificationListener",
                 &AccessibilityControllerBindings::SetNotificationListener)
      .SetMethod("unsetNotificationListener",
                 &AccessibilityControllerBindings::UnsetNotificationListener)
      .SetProperty("focusedElement",
                   &AccessibilityControllerBindings::FocusedElement)
      .SetProperty("rootElement", &AccessibilityControllerBindings::RootElement)
      .SetMethod("accessibleElementById",
                 &AccessibilityControllerBindings::AccessibleElementById)
      // TODO(hajimehoshi): These are for backward compatibility. Remove them.
      .SetMethod("addNotificationListener",
                 &AccessibilityControllerBindings::SetNotificationListener)
      .SetMethod("removeNotificationListener",
                 &AccessibilityControllerBindings::UnsetNotificationListener);
}

void AccessibilityControllerBindings::LogAccessibilityEvents() {
  if (controller_)
    controller_->LogAccessibilityEvents();
}

void AccessibilityControllerBindings::SetNotificationListener(
    v8::Local<v8::Function> callback) {
  if (controller_)
    controller_->SetNotificationListener(callback);
}

void AccessibilityControllerBindings::UnsetNotificationListener() {
  if (controller_)
    controller_->UnsetNotificationListener();
}

v8::Local<v8::Object> AccessibilityControllerBindings::FocusedElement() {
  return controller_ ? controller_->FocusedElement() : v8::Local<v8::Object>();
}

v8::Local<v8::Object> AccessibilityControllerBindings::RootElement() {
  return controller_ ? controller_->RootElement() : v8::Local<v8::Object>();
}

v8::Local<v8::Object> AccessibilityControllerBindings::AccessibleElementById(
    const std::string& id) {
  return controller_ ? controller_->AccessibleElementById(id)
                     : v8::Local<v8::Object>();
}

AccessibilityController::AccessibilityController(
    WebViewTestProxy* web_view_test_proxy)
    : log_accessibility_events_(false),
      web_view_test_proxy_(web_view_test_proxy),
      weak_factory_(this) {}

AccessibilityController::~AccessibilityController() {}

void AccessibilityController::Reset() {
  elements_.Clear();
  notification_callback_.Reset();
  log_accessibility_events_ = false;
}

void AccessibilityController::Install(blink::WebLocalFrame* frame) {
  ax_context_.reset(new blink::WebAXContext(frame->GetDocument()));
  frame->View()->GetSettings()->SetInlineTextBoxAccessibilityEnabled(true);

  AccessibilityControllerBindings::Install(weak_factory_.GetWeakPtr(), frame);
}

bool AccessibilityController::ShouldLogAccessibilityEvents() {
  return log_accessibility_events_;
}

void AccessibilityController::NotificationReceived(
    const blink::WebAXObject& target,
    const std::string& notification_name) {
  v8::Isolate* isolate = blink::MainThreadIsolate();
  v8::HandleScope handle_scope(isolate);

  blink::WebFrame* frame = web_view()->MainFrame();
  if (!frame || frame->IsWebRemoteFrame())
    return;
  blink::WebLocalFrame* local_frame = frame->ToWebLocalFrame();

  v8::Local<v8::Context> context = local_frame->MainWorldScriptContext();
  if (context.IsEmpty())
    return;

  v8::Context::Scope context_scope(context);

  // Call notification listeners on the element.
  v8::Local<v8::Object> element_handle = elements_.GetOrCreate(target);
  if (element_handle.IsEmpty())
    return;

  WebAXObjectProxy* element;
  bool result = gin::ConvertFromV8(isolate, element_handle, &element);
  DCHECK(result);
  element->NotificationReceived(local_frame, notification_name);

  if (notification_callback_.IsEmpty())
    return;

  // Call global notification listeners.
  v8::Local<v8::Value> argv[] = {
      element_handle,
      v8::String::NewFromUtf8(isolate, notification_name.data(),
                              v8::NewStringType::kNormal,
                              notification_name.size())
          .ToLocalChecked(),
  };
  local_frame->CallFunctionEvenIfScriptDisabled(
      v8::Local<v8::Function>::New(isolate, notification_callback_),
      context->Global(), base::size(argv), argv);
}

void AccessibilityController::LogAccessibilityEvents() {
  log_accessibility_events_ = true;
}

void AccessibilityController::SetNotificationListener(
    v8::Local<v8::Function> callback) {
  v8::Isolate* isolate = blink::MainThreadIsolate();
  notification_callback_.Reset(isolate, callback);
}

void AccessibilityController::UnsetNotificationListener() {
  notification_callback_.Reset();
}

v8::Local<v8::Object> AccessibilityController::FocusedElement() {
  blink::WebFrame* frame = web_view()->MainFrame();
  if (!frame)
    return v8::Local<v8::Object>();

  // TODO(lukasza): Finish adding OOPIF support to the web tests harness.
  CHECK(frame->IsWebLocalFrame())
      << "This function cannot be called if the main frame is not a "
         "local frame.";
  blink::WebAXObject focused_element =
      blink::WebAXObject::FromWebDocumentFocused(
          frame->ToWebLocalFrame()->GetDocument());
  if (focused_element.IsNull())
    focused_element = GetAccessibilityObjectForMainFrame();
  return elements_.GetOrCreate(focused_element);
}

v8::Local<v8::Object> AccessibilityController::RootElement() {
  return elements_.GetOrCreate(GetAccessibilityObjectForMainFrame());
}

v8::Local<v8::Object> AccessibilityController::AccessibleElementById(
    const std::string& id) {
  blink::WebAXObject root_element = GetAccessibilityObjectForMainFrame();

  if (!root_element.UpdateLayoutAndCheckValidity())
    return v8::Local<v8::Object>();

  return FindAccessibleElementByIdRecursive(
      root_element, blink::WebString::FromUTF8(id.c_str()));
}

v8::Local<v8::Object>
AccessibilityController::FindAccessibleElementByIdRecursive(
    const blink::WebAXObject& obj,
    const blink::WebString& id) {
  if (obj.IsNull() || obj.IsDetached())
    return v8::Local<v8::Object>();

  blink::WebNode node = obj.GetNode();
  if (!node.IsNull() && node.IsElementNode()) {
    blink::WebElement element = node.To<blink::WebElement>();
    if (element.GetAttribute("id") == id)
      return elements_.GetOrCreate(obj);
  }

  unsigned childCount = obj.ChildCount();
  for (unsigned i = 0; i < childCount; i++) {
    v8::Local<v8::Object> result =
        FindAccessibleElementByIdRecursive(obj.ChildAt(i), id);
    if (!result.IsEmpty())
      return result;
  }

  return v8::Local<v8::Object>();
}

blink::WebView* AccessibilityController::web_view() {
  return web_view_test_proxy_->webview();
}

blink::WebAXObject
AccessibilityController::GetAccessibilityObjectForMainFrame() {
  blink::WebFrame* frame = web_view()->MainFrame();

  // TODO(lukasza): Finish adding OOPIF support to the web tests harness.
  CHECK(frame && frame->IsWebLocalFrame())
      << "This function cannot be called if the main frame is not a "
         "local frame.";
  return blink::WebAXObject::FromWebDocument(
      web_view()->MainFrame()->ToWebLocalFrame()->GetDocument());
}

}  // namespace test_runner

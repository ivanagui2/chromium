// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/message_center/views/message_popup_collection.h"

#include <set>

#include "base/bind.h"
#include "base/timer.h"
#include "ui/gfx/screen.h"
#include "ui/message_center/message_center.h"
#include "ui/message_center/message_center_constants.h"
#include "ui/message_center/notification.h"
#include "ui/message_center/notification_list.h"
#include "ui/message_center/views/message_view.h"
#include "ui/message_center/views/notification_view.h"
#include "ui/views/background.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"

namespace message_center {

class ToastContentsView : public views::WidgetDelegateView {
 public:
  ToastContentsView(const Notification* notification,
                    MessagePopupCollection* collection)
      : collection_(collection) {
    DCHECK(collection_);

    set_notify_enter_exit_on_child(true);
    SetLayoutManager(new views::FillLayout());
    // Sets the transparent background. Then, when the message view is slid out,
    // the whole toast seems to slide although the actual bound of the widget
    // remains. This is hacky but easier to keep the consistency.
    set_background(views::Background::CreateSolidBackground(0, 0, 0, 0));

    int seconds = kAutocloseDefaultDelaySeconds;
    if (notification->priority() > DEFAULT_PRIORITY)
      seconds = kAutocloseHighPriorityDelaySeconds;
    delay_ = base::TimeDelta::FromSeconds(seconds);
  }

  views::Widget* CreateWidget(gfx::NativeView context) {
    views::Widget::InitParams params(
        views::Widget::InitParams::TYPE_WINDOW_FRAMELESS);
    params.keep_on_top = true;
    params.context = context;
    params.transparent = true;
    // The origin of the initial bounds are set to (0, 0). It'll then moved by
    // MessagePopupCollection.
    params.bounds = gfx::Rect(
        gfx::Size(kWebNotificationWidth, GetPreferredSize().height()));
    params.delegate = this;
    views::Widget* widget = new views::Widget();
    widget->Init(params);
    return widget;
  }

  void SetContents(MessageView* view) {
    RemoveAllChildViews(true);
    AddChildView(view);
    Layout();
  }

  void SuspendTimer() {
    timer_.Reset();
  }

  void RestartTimer() {
    base::TimeDelta passed = base::Time::Now() - start_time_;
    if (passed > delay_) {
      GetWidget()->Close();
    } else {
      delay_ -= passed;
      StartTimer();
    }
  }

  void StartTimer() {
    start_time_ = base::Time::Now();
    timer_.Start(FROM_HERE,
                 delay_,
                 base::Bind(&views::Widget::Close,
                            base::Unretained(GetWidget())));
  }

  // Overridden from views::WidgetDelegate:
  virtual views::View* GetContentsView() OVERRIDE {
    return this;
  }

  virtual void WindowClosing() OVERRIDE {
    if (timer_.IsRunning())
      SuspendTimer();
  }

  virtual bool CanActivate() const OVERRIDE {
    return false;
  }

  // Overridden from views::View:
  virtual void OnMouseEntered(const ui::MouseEvent& event) OVERRIDE {
    collection_->OnMouseEntered();
  }

  virtual void OnMouseExited(const ui::MouseEvent& event) OVERRIDE {
    collection_->OnMouseExited();
  }

 private:
  base::TimeDelta delay_;
  base::Time start_time_;
  base::OneShotTimer<views::Widget> timer_;
  MessagePopupCollection* collection_;

  DISALLOW_COPY_AND_ASSIGN(ToastContentsView);
};

MessagePopupCollection::MessagePopupCollection(gfx::NativeView context,
                                               MessageCenter* message_center)
    : context_(context),
      message_center_(message_center) {
  DCHECK(message_center_);
}

MessagePopupCollection::~MessagePopupCollection() {
  CloseAllWidgets();
}

void MessagePopupCollection::UpdatePopups() {
  NotificationList::PopupNotifications popups =
      message_center_->notification_list()->GetPopupNotifications();

  if (popups.empty()) {
    CloseAllWidgets();
    return;
  }

  gfx::Screen* screen = gfx::Screen::GetScreenFor(context_);
  gfx::Rect work_area = screen->GetDisplayNearestWindow(context_).work_area();

  std::set<std::string> old_toast_ids;
  for (ToastContainer::iterator iter = toasts_.begin(); iter != toasts_.end();
       ++iter) {
    old_toast_ids.insert(iter->first);
  }

  int total_height = 0;
  std::vector<views::Widget*> widgets;
  for (NotificationList::PopupNotifications::const_iterator iter =
           popups.begin(); iter != popups.end(); ++iter) {
    ToastContainer::iterator toast_iter = toasts_.find((*iter)->id());
    views::Widget* widget = NULL;
    // NotificationViews are expanded by default here because
    // MessagePopupCollection hasn't been tested yet with changing subview
    // sizes, and such changes could come if those subviews were initially
    // collapsed and allowed to be expanded by users. TODO(dharcourt): Fix.
    MessageView* view = NotificationView::Create(*(*iter), message_center_,
                                                 true);
    if (toast_iter != toasts_.end()) {
      widget = toast_iter->second->GetWidget();
      old_toast_ids.erase((*iter)->id());
      // Need to replace the contents because |view| can be updated, like
      // image loads.
      toast_iter->second->SetContents(view);
    } else {
      ToastContentsView* toast = new ToastContentsView(*iter, this);
      toast->SetContents(view);
      widget = toast->CreateWidget(context_);
      widget->AddObserver(this);
      toast->StartTimer();
      toasts_[(*iter)->id()] = toast;
    }
    if (widget) {
      gfx::Rect bounds = widget->GetWindowBoundsInScreen();
      int new_height = total_height + bounds.height() + kMarginBetweenItems;
      if (new_height < work_area.height()) {
        total_height = new_height;
        widgets.push_back(widget);
      } else {
        if (toast_iter != toasts_.end())
          toasts_.erase(toast_iter);
        delete widget;
        break;
      }
    }
  }

  for (std::set<std::string>::const_iterator iter = old_toast_ids.begin();
       iter != old_toast_ids.end(); ++iter) {
    ToastContainer::iterator toast_iter = toasts_.find(*iter);
    DCHECK(toast_iter != toasts_.end());
    views::Widget* widget = toast_iter->second->GetWidget();
    widget->RemoveObserver(this);
    widget->Close();
    toasts_.erase(toast_iter);
  }

  // Place/move the toast widgets. Currently it stacks the widgets from the
  // right-bottom of the work area.
  // TODO(mukai): allow to specify the placement policy from outside of this
  // class. The policy should be specified from preference on Windows, or
  // the launcher alignment on ChromeOS.
  int top = work_area.bottom() - total_height;
  int left = work_area.right() - kWebNotificationWidth - kMarginBetweenItems;
  for (size_t i = 0; i < widgets.size(); ++i) {
    gfx::Rect widget_bounds = widgets[i]->GetWindowBoundsInScreen();
    widgets[i]->SetBounds(gfx::Rect(
        left, top, widget_bounds.width(), widget_bounds.height()));
    if (!widgets[i]->IsVisible())
      widgets[i]->Show();
    top += widget_bounds.height() + kMarginBetweenItems;
  }
}

void MessagePopupCollection::OnMouseEntered() {
  for (ToastContainer::iterator iter = toasts_.begin();
       iter != toasts_.end(); ++iter) {
    iter->second->SuspendTimer();
  }
}

void MessagePopupCollection::OnMouseExited() {
  for (ToastContainer::iterator iter = toasts_.begin();
       iter != toasts_.end(); ++iter) {
    iter->second->RestartTimer();
  }
}

void MessagePopupCollection::CloseAllWidgets() {
  for (ToastContainer::iterator iter = toasts_.begin();
       iter != toasts_.end(); ++iter) {
    iter->second->SuspendTimer();
    views::Widget* widget = iter->second->GetWidget();
    widget->RemoveObserver(this);
    widget->Close();
  }
  toasts_.clear();
}

void MessagePopupCollection::OnWidgetDestroying(views::Widget* widget) {
  widget->RemoveObserver(this);
  for (ToastContainer::iterator iter = toasts_.begin();
       iter != toasts_.end(); ++iter) {
    if (iter->second->GetWidget() == widget) {
      message_center_->notification_list()->MarkSinglePopupAsShown(
          iter->first, false);
      toasts_.erase(iter);
      break;
    }
  }
  UpdatePopups();
}

}  // namespace message_center

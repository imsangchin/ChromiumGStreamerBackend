// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MANDOLINE_UI_OMNIBOX_OMNIBOX_IMPL_H_
#define MANDOLINE_UI_OMNIBOX_OMNIBOX_IMPL_H_

#include "components/view_manager/public/cpp/view_tree_delegate.h"
#include "mandoline/ui/desktop_ui/public/interfaces/omnibox.mojom.h"
#include "mandoline/ui/desktop_ui/public/interfaces/view_embedder.mojom.h"
#include "mojo/application/public/cpp/application_delegate.h"
#include "mojo/application/public/cpp/interface_factory.h"
#include "mojo/common/weak_binding_set.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/layout/layout_manager.h"

namespace mojo {
class ViewTreeClientFactory;
}

namespace mandoline {

class AuraInit;

class OmniboxImpl : public mojo::ApplicationDelegate,
                    public mojo::ViewTreeDelegate,
                    public views::LayoutManager,
                    public views::TextfieldController,
                    public mojo::InterfaceFactory<Omnibox>,
                    public mojo::InterfaceFactory<mojo::ViewTreeClient>,
                    public Omnibox {
 public:
  OmniboxImpl();
  ~OmniboxImpl() override;

 private:
  // Overridden from mojo::ApplicationDelegate:
  void Initialize(mojo::ApplicationImpl* app) override;
  bool ConfigureIncomingConnection(
      mojo::ApplicationConnection* connection) override;
  bool ConfigureOutgoingConnection(
      mojo::ApplicationConnection* connection) override;

  // Overridden from mojo::ViewTreeDelegate:
  void OnEmbed(mojo::View* root) override;
  void OnConnectionLost(mojo::ViewTreeConnection* connection) override;

  // Overridden from views::LayoutManager:
  gfx::Size GetPreferredSize(const views::View* view) const override;
  void Layout(views::View* host) override;

  // Overridden from views::TextfieldController:
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override;

  // Overridden from mojo::InterfaceFactory<Omnibox>:
  void Create(mojo::ApplicationConnection* connection,
              mojo::InterfaceRequest<Omnibox> request) override;

  // Overridden from mojo::InterfaceFactory<mojo::ViewTreeClient>:
  void Create(
      mojo::ApplicationConnection* connection,
      mojo::InterfaceRequest<mojo::ViewTreeClient> request) override;

  // Overridden from Omnibox:
  void ShowForURL(const mojo::String& url) override;

  void HideWindow();
  void ShowWindow();

  scoped_ptr<AuraInit> aura_init_;
  mojo::ApplicationImpl* app_impl_;
  mojo::View* root_;
  mojo::String url_;
  views::Textfield* edit_;
  mojo::WeakBindingSet<Omnibox> bindings_;
  ViewEmbedderPtr view_embedder_;

  DISALLOW_COPY_AND_ASSIGN(OmniboxImpl);
};

}  // namespace mandoline

#endif  // MANDOLINE_UI_OMNIBOX_OMNIBOX_IMPL_H_

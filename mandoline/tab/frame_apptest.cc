// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mandoline/tab/frame.h"

#include "base/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/test/test_timeouts.h"
#include "components/view_manager/public/cpp/view_manager_init.h"
#include "components/view_manager/public/cpp/view_observer.h"
#include "components/view_manager/public/cpp/view_tree_connection.h"
#include "components/view_manager/public/cpp/view_tree_delegate.h"
#include "mandoline/tab/frame.h"
#include "mandoline/tab/frame_tree.h"
#include "mandoline/tab/frame_tree_delegate.h"
#include "mandoline/tab/frame_user_data.h"
#include "mandoline/tab/test_frame_tree_delegate.h"
#include "mojo/application/public/cpp/application_connection.h"
#include "mojo/application/public/cpp/application_delegate.h"
#include "mojo/application/public/cpp/application_impl.h"
#include "mojo/application/public/cpp/application_test_base.h"
#include "mojo/application/public/cpp/service_provider_impl.h"

using mojo::View;
using mojo::ViewTreeConnection;

namespace mandoline {

namespace {

base::RunLoop* current_run_loop = nullptr;

void TimeoutRunLoop(const base::Closure& timeout_task, bool* timeout) {
  CHECK(current_run_loop);
  *timeout = true;
  timeout_task.Run();
}

bool DoRunLoopWithTimeout() {
  if (current_run_loop != nullptr)
    return false;

  bool timeout = false;
  base::RunLoop run_loop;
  base::MessageLoop::current()->PostDelayedTask(
      FROM_HERE, base::Bind(&TimeoutRunLoop, run_loop.QuitClosure(), &timeout),
      TestTimeouts::action_timeout());

  current_run_loop = &run_loop;
  current_run_loop->Run();
  current_run_loop = nullptr;
  return !timeout;
}

void QuitRunLoop() {
  current_run_loop->Quit();
  current_run_loop = nullptr;
}

}  // namespace

class FrameTest : public mojo::test::ApplicationTestBase,
                  public mojo::ApplicationDelegate,
                  public mojo::ViewTreeDelegate,
                  public mojo::InterfaceFactory<mojo::ViewTreeClient> {
 public:
  FrameTest() : most_recent_connection_(nullptr), window_manager_(nullptr) {}

  ViewTreeConnection* most_recent_connection() {
    return most_recent_connection_;
  }

  // ApplicationDelegate implementation.
  bool ConfigureIncomingConnection(
      mojo::ApplicationConnection* connection) override {
    connection->AddService<mojo::ViewTreeClient>(this);
    return true;
  }

  ViewTreeConnection* window_manager() { return window_manager_; }

  // ApplicationTestBase:
  ApplicationDelegate* GetApplicationDelegate() override { return this; }

  // Overridden from ViewTreeDelegate:
  void OnEmbed(View* root) override {
    most_recent_connection_ = root->connection();
    QuitRunLoop();
  }
  void OnConnectionLost(ViewTreeConnection* connection) override {}

 private:
  // Overridden from testing::Test:
  void SetUp() override {
    ApplicationTestBase::SetUp();

    view_manager_init_.reset(
        new mojo::ViewManagerInit(application_impl(), this, nullptr));
    ASSERT_TRUE(DoRunLoopWithTimeout());
    std::swap(window_manager_, most_recent_connection_);
  }

  // Overridden from testing::Test:
  void TearDown() override {
    view_manager_init_.reset();  // Uses application_impl() from base class.
    ApplicationTestBase::TearDown();
  }

  // Overridden from mojo::InterfaceFactory<mojo::ViewTreeClient>:
  void Create(
      mojo::ApplicationConnection* connection,
      mojo::InterfaceRequest<mojo::ViewTreeClient> request) override {
    mojo::ViewTreeConnection::Create(this, request.Pass());
  }

  scoped_ptr<mojo::ViewManagerInit> view_manager_init_;

  // Used to receive the most recent view manager loaded by an embed action.
  ViewTreeConnection* most_recent_connection_;
  // The View Manager connection held by the window manager (app running at the
  // root view).
  ViewTreeConnection* window_manager_;

  MOJO_DISALLOW_COPY_AND_ASSIGN(FrameTest);
};

class TestFrameTreeClient : public FrameTreeClient {
 public:
  TestFrameTreeClient() : connect_count_(0) {}
  ~TestFrameTreeClient() override {}

  int connect_count() const { return connect_count_; }

  mojo::Array<FrameDataPtr> connect_frames() { return connect_frames_.Pass(); }

  mojo::Array<FrameDataPtr> adds() { return adds_.Pass(); }

  // TestFrameTreeClient:
  void OnConnect(FrameTreeServerPtr server,
                 uint32_t change_id,
                 mojo::Array<FrameDataPtr> frames) override {
    connect_count_++;
    connect_frames_ = frames.Pass();
    server_ = server.Pass();
  }
  void OnFrameAdded(uint32_t change_id, FrameDataPtr frame) override {
    adds_.push_back(frame.Pass());
  }
  void OnFrameRemoved(uint32_t change_id, uint32_t frame_id) override {}
  void OnFrameClientPropertyChanged(uint32_t frame_id,
                                    const mojo::String& name,
                                    mojo::Array<uint8_t> new_data) override {}
  void OnPostMessageEvent(uint32_t source_frame_id,
                          uint32_t target_frame_id,
                          HTMLMessageEventPtr event) override {}
  void OnWillNavigate(uint32_t frame_id,
                      const OnWillNavigateCallback& callback) override {
    callback.Run();
  }

 private:
  int connect_count_;
  mojo::Array<FrameDataPtr> connect_frames_;
  FrameTreeServerPtr server_;
  mojo::Array<FrameDataPtr> adds_;

  DISALLOW_COPY_AND_ASSIGN(TestFrameTreeClient);
};

// Verifies the root gets a connect.
TEST_F(FrameTest, RootGetsConnect) {
  TestFrameTreeDelegate tree_delegate;
  TestFrameTreeClient root_client;
  FrameTree tree(window_manager()->GetRoot(), &tree_delegate, &root_client,
                 nullptr, Frame::ClientPropertyMap());
  ASSERT_EQ(1, root_client.connect_count());
  mojo::Array<FrameDataPtr> frames = root_client.connect_frames();
  ASSERT_EQ(1u, frames.size());
  EXPECT_EQ(tree.root()->view()->id(), frames[0]->frame_id);
  EXPECT_EQ(0u, frames[0]->parent_id);
}

// Verifies adding a child to the root.
TEST_F(FrameTest, SingleChild) {
  TestFrameTreeDelegate tree_delegate;
  TestFrameTreeClient root_client;
  FrameTree tree(window_manager()->GetRoot(), &tree_delegate, &root_client,
                 nullptr, Frame::ClientPropertyMap());

  View* child = window_manager()->CreateView();
  EXPECT_EQ(nullptr, Frame::FindFirstFrameAncestor(child));
  window_manager()->GetRoot()->AddChild(child);
  EXPECT_EQ(tree.root(), Frame::FindFirstFrameAncestor(child));

  TestFrameTreeClient child_client;
  Frame* child_frame =
      tree.CreateAndAddFrame(child, tree.root(), &child_client, nullptr);
  EXPECT_EQ(tree.root(), child_frame->parent());

  ASSERT_EQ(1, child_client.connect_count());
  mojo::Array<FrameDataPtr> frames_in_child = child_client.connect_frames();
  // We expect 2 frames. One for the root, one for the child.
  ASSERT_EQ(2u, frames_in_child.size());
  EXPECT_EQ(tree.root()->view()->id(), frames_in_child[0]->frame_id);
  EXPECT_EQ(0u, frames_in_child[0]->parent_id);
  EXPECT_EQ(child_frame->view()->id(), frames_in_child[1]->frame_id);
  EXPECT_EQ(tree.root()->view()->id(), frames_in_child[1]->parent_id);

  // We should have gotten notification of the add.
  EXPECT_EQ(1u, root_client.adds().size());
}

}  // namespace mandoline

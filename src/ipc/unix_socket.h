/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_IPC_UNIX_SOCKET_H_
#define SRC_IPC_UNIX_SOCKET_H_

#include <stdint.h>
#include <sys/types.h>

#include <memory>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/weak_ptr.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base.

namespace ipc {

// A non-blocking UNIX domain socket in SOCK_STREAM mode. Allows also to
// transfer file descriptors. None of the methods in this class are blocking.
// The main design goal is API simplicity and strong guarantees on the
// EventListener callbacks, in order to avoid ending in some undefined state.
// In case of any error it will aggressively just shut down the socket and
// notify the failure with OnConnect(false) or OnDisconnect() depending on the
// state of the socket (see below).
// EventListener callbacks stop happening as soon as the instance is destroyed.
//
// Lifecycle of a client socket:
//
//                           Connect()
//                               |
//            +------------------+------------------+
//            | (success)                           | (failure or Shutdown())
//            V                                     V
//     OnConnect(true)                         OnConnect(false)
//            |
//            V
//    OnDataAvailable()
//            |
//            V
//     OnDisconnect()  (failure or shutdown)
//
//
// Lifecycle of a server socket:
//
//                          Listen()  --> returns false in case of errors.
//                             |
//                             V
//              OnNewIncomingConnection(new_socket)
//
//          (|new_socket| inherits the same EventListener)
//                             |
//                             V
//                     OnDataAvailable()
//                             | (failure or Shutdown())
//                             V
//                       OnDisconnect()
class UnixSocket {
 public:
  class EventListener {
   public:
    virtual ~EventListener();

    // After Listen().
    virtual void OnNewIncomingConnection(
        UnixSocket* self,
        std::unique_ptr<UnixSocket> new_connection);

    // After Connect(), whether successful or not.
    virtual void OnConnect(UnixSocket* self, bool connected);

    // After a successful Connect() or OnNewIncomingConnection(). Either the
    // other endpoint did disconnect or some other error happened.
    virtual void OnDisconnect(UnixSocket* self);

    // Whenever there is data available to Receive(). Note that spurious FD
    // watch events are possible, so it is possible that Receive() soon after
    // OnDataAvailable() returns 0 (just ignore those).
    virtual void OnDataAvailable(UnixSocket* self);
  };

  enum class State {
    kDisconnected = 0,  // Failed connection, peer disconnection or Shutdown().
    kConnecting,  // Soon after Connect(), before it either succeeds or fails.
    kConnected,   // After a successful Connect().
    kListening    // After Listen(), until Shutdown().
  };

  enum class BlockingMode { kNonBlocking, kBlocking };

  // Creates a Unix domain socket and starts listening. If |socket_name|
  // starts with a '@', an abstract socket will be created (Linux/Android only).
  // Returns always an instance. In case of failure (e.g., another socket
  // with the same name is  already listening) the returned socket will have
  // is_listening() == false and last_error() will contain the failure reason.
  static std::unique_ptr<UnixSocket> Listen(const std::string& socket_name,
                                            EventListener*,
                                            base::TaskRunner*);

  // Creates a Unix domain socket and connects to the listening endpoint.
  // Returns always an instance. EventListener::OnConnect(bool success) will
  // be called always, whether the connection succeeded or not.
  static std::unique_ptr<UnixSocket> Connect(const std::string& socket_name,
                                             EventListener*,
                                             base::TaskRunner*);

  // This class gives the hard guarantee that no callback is called on the
  // passed EventListener immediately after the object has been destroyed.
  // Any queued callback will be silently dropped.
  ~UnixSocket();

  // Shuts down the current connection, if any. If the socket was Listen()-ing,
  // stops listening. The socket goes back to kNotInitialized state, so it can
  // be reused with Listen() or Connect().
  void Shutdown();

  // Returns true is the message was queued, false if there was no space in the
  // output buffer, in which case the client should retry or give up.
  // If any other error happens the socket will be shutdown and
  // EventListener::OnDisconnect() will be called.
  // If the socket is not connected, Send() will just return false.
  // Does not append a null string terminator to msg in any case.
  bool Send(const void* msg,
            size_t len,
            int send_fd = -1,
            BlockingMode blocking = BlockingMode::kNonBlocking);
  bool Send(const std::string& msg);

  // Returns the number of bytes (<= |len|) written in |msg| or 0 if there
  // is no data in the buffer to read or an error occurs (in which case a
  // EventListener::OnDisconnect() will follow).
  // If the ScopedFile pointer is not null and a FD is received, it moves the
  // received FD into that. If a FD is received but the ScopedFile pointer is
  // null, the FD will be automatically closed.
  size_t Receive(void* msg, size_t len, base::ScopedFile* = nullptr);

  // Only for tests. This is slower than Receive() as it requires a heap
  // allocation and a copy for the std::string. Guarantees that the returned
  // string is null terminated even if the underlying message sent by the peer
  // is not.
  std::string ReceiveString(size_t max_length = 1024);

  bool is_connected() const { return state_ == State::kConnected; }
  bool is_listening() const { return state_ == State::kListening; }
  int fd() const { return fd_.get(); }
  int last_error() const { return last_error_; }

  // User ID of the peer, as returned by the kernel. If the client disconnects
  // and the socket goes into the kDisconnected state, it retains the uid of
  // the last peer.
  int peer_uid() const {
    PERFETTO_DCHECK(!is_listening() && peer_uid_ >= 0);
    return peer_uid_;
  }

 private:
  UnixSocket(EventListener*, base::TaskRunner*);
  UnixSocket(EventListener*, base::TaskRunner*, base::ScopedFile);
  UnixSocket(const UnixSocket&) = delete;
  UnixSocket& operator=(const UnixSocket&) = delete;

  // Called once by the corresponding public static factory methods.
  void DoConnect(const std::string& socket_name);
  void DoListen(const std::string& socket_name);
  void ReadPeerCredentials();
  void SetBlockingIO(bool is_blocking);

  void OnEvent();
  void NotifyConnectionState(bool success);

  base::ScopedFile fd_;
  State state_ = State::kDisconnected;
  int last_error_ = 0;
  int peer_uid_ = -1;
  EventListener* event_listener_;
  base::TaskRunner* task_runner_;
  base::WeakPtrFactory<UnixSocket> weak_ptr_factory_;
};

}  // namespace ipc
}  // namespace perfetto

#endif  // SRC_IPC_UNIX_SOCKET_H_

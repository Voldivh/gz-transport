/*
 * Copyright (C) 2014 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <algorithm>
#include <cassert>
#include <csignal>
#include <condition_variable>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <future>
#include <thread>

#include "ignition/transport/MessageInfo.hh"
#include "ignition/transport/Node.hh"
#include "ignition/transport/NodeOptions.hh"
#include "ignition/transport/NodePrivate.hh"
#include "ignition/transport/NodeShared.hh"
#include "ignition/transport/TopicUtils.hh"
#include "ignition/transport/TransportTypes.hh"
#include "ignition/transport/Uuid.hh"

#include "NodeSharedPrivate.hh"

#ifdef _MSC_VER
#pragma warning(disable: 4503)
#endif

using namespace ignition;
using namespace transport;

namespace ignition
{
  namespace transport
  {
    /// \brief Flag to detect SIGINT or SIGTERM while the code is executing
    /// waitForShutdown().
    static bool g_shutdown = false;

    /// \brief Mutex to protect the boolean shutdown variable.
    static std::mutex g_shutdown_mutex;

    /// \brief Condition variable to wakeup waitForShutdown() and exit.
    static std::condition_variable g_shutdown_cv;

    //////////////////////////////////////////////////
    /// \brief Function executed when a SIGINT or SIGTERM signals are captured.
    /// \param[in] _signal Signal received.
    static void signal_handler(const int _signal)
    {
      if (_signal == SIGINT || _signal == SIGTERM)
      {
        g_shutdown_mutex.lock();
        g_shutdown = true;
        g_shutdown_mutex.unlock();
        g_shutdown_cv.notify_all();
      }
    }

    //////////////////////////////////////////////////
    /// \internal
    /// \brief Private data for Node::Publisher class.
    class Node::PublisherPrivate
    {
      /// \brief Default constructor.
      public: PublisherPrivate()
        : shared(NodeShared::Instance())
      {
      }

      /// \brief Constructor
      /// \param[in] _publisher The message publisher.
      public: explicit PublisherPrivate(const MessagePublisher &_publisher)
        : shared(NodeShared::Instance()),
          publisher(_publisher)
      {
      }

      /// \brief Destructor.
      public: virtual ~PublisherPrivate()
      {
        std::lock_guard<std::recursive_mutex> lk(this->shared->mutex);
        // Notify the discovery service to unregister and unadvertise my topic.
        if (!this->shared->dataPtr->msgDiscovery->Unadvertise(
               this->publisher.Topic(), this->publisher.NUuid()))
        {
          std::cerr << "~PublisherPrivate() Error unadvertising topic ["
                    << this->publisher.Topic() << "]" << std::endl;
        }
      }

      /// \brief Pointer to the object shared between all the nodes within the
      /// same process.
      public: NodeShared *shared = nullptr;

      /// \brief The message publisher.
      public: MessagePublisher publisher;

      /// \brief Timestamp of the last callback executed.
      public: Timestamp lastCbTimestamp;

      /// \brief If throttling is enabled, the minimum period for receiving a
      /// message in nanoseconds.
      public: double periodNs = 0.0;

      /// \brief The topic name without the partition part.
      public: std::string topicWithoutPartition = "";

      /// \brief Mutex to protect the node::publisher from race conditions.
      public: std::mutex mutex;
    };
  }
}

//////////////////////////////////////////////////
void ignition::transport::waitForShutdown()
{
  // Install a signal handler for SIGINT and SIGTERM.
  std::signal(SIGINT,  signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::unique_lock<std::mutex> lk(g_shutdown_mutex);
  g_shutdown_cv.wait(lk, []{return g_shutdown;});
}

//////////////////////////////////////////////////
Node::Publisher::Publisher()
  : dataPtr(std::make_shared<PublisherPrivate>())
{
}

//////////////////////////////////////////////////
Node::Publisher::Publisher(const MessagePublisher &_publisher)
  : dataPtr(std::make_shared<PublisherPrivate>(_publisher))
{
  if (!this->Valid())
    return;

  // Get the topic and remove the partition name.
  std::string topic = this->dataPtr->publisher.Topic();
  topic.erase(0, topic.find_last_of("@") + 1);
  this->dataPtr->topicWithoutPartition = topic;

  if (this->dataPtr->publisher.Options().Throttled())
  {
    this->dataPtr->periodNs =
      1e9 / this->dataPtr->publisher.Options().MsgsPerSec();
  }
}

//////////////////////////////////////////////////
Node::Publisher::~Publisher()
{
}

//////////////////////////////////////////////////
Node::Publisher::operator bool()
{
  return this->Valid();
}

//////////////////////////////////////////////////
Node::Publisher::operator bool() const
{
  return this->Valid();
}

//////////////////////////////////////////////////
bool Node::Publisher::Valid() const
{
  return !this->dataPtr->publisher.Topic().empty();
}

//////////////////////////////////////////////////
bool Node::Publisher::HasConnections() const
{
  ISubscriptionHandlerPtr firstSubscriberPtr;
  auto &publisher = this->dataPtr->publisher;

  std::lock_guard<std::recursive_mutex> lk(this->dataPtr->shared->mutex);

  return this->Valid() &&
    (this->dataPtr->shared->localSubscriptions.FirstHandler(
       publisher.Topic(), publisher.MsgTypeName(), firstSubscriberPtr) ||
     this->dataPtr->shared->remoteSubscribers.HasTopic(
       publisher.Topic(), publisher.MsgTypeName()));
}

//////////////////////////////////////////////////
bool Node::Publisher::Precheck(const ProtoMsg &_msg) const
{
  if (!this->Valid())
    return false;

  // Check that the msg type matches the topic type previously advertised.
  if (this->dataPtr->publisher.MsgTypeName() != _msg.GetTypeName())
  {
    std::cerr << "Node::Publisher::Precheck() Type mismatch.\n"
              << "\t* Type advertised: "
              << this->dataPtr->publisher.MsgTypeName()
              << "\n\t* Type published: " << _msg.GetTypeName() << std::endl;
    return false;
  }

  return true;
}

//////////////////////////////////////////////////
bool Node::Publisher::CheckSubscribers(
  const ProtoMsg &_msg, bool &_hasLocalSubscribers,
  std::map<std::string, ISubscriptionHandler_M> &_handlers,
  bool &_hasRemoteSubscribers) const
{
  {
    std::lock_guard<std::recursive_mutex> lk(this->dataPtr->shared->mutex);

    _hasLocalSubscribers = this->dataPtr->shared->localSubscriptions.Handlers(
      this->dataPtr->publisher.Topic(), _handlers);
    _hasRemoteSubscribers = this->dataPtr->shared->remoteSubscribers.HasTopic(
      this->dataPtr->publisher.Topic(), _msg.GetTypeName());
  }

  return _hasLocalSubscribers || _hasRemoteSubscribers;
}

//////////////////////////////////////////////////
void Node::Publisher::SendToLocalSubscribers(
  const std::map<std::string, ISubscriptionHandler_M> &_handlers,
  const ProtoMsg &_msg) const
{
  MessageInfo info;
  info.SetTopic(this->dataPtr->topicWithoutPartition);

  for (auto &node : _handlers)
  {
    for (auto &handler : node.second)
    {
      ISubscriptionHandlerPtr subscriptionHandlerPtr = handler.second;

      if (subscriptionHandlerPtr)
      {
        if (subscriptionHandlerPtr->TypeName() != kGenericMessageType &&
            subscriptionHandlerPtr->TypeName() != _msg.GetTypeName())
        {
          continue;
        }
        subscriptionHandlerPtr->RunLocalCallback(_msg, info);
      }
      else
      {
        std::cerr << "Node::Publisher::Publish(): NULL subscription handler"
                  << std::endl;
      }
    }
  }
}

//////////////////////////////////////////////////
bool Node::Publisher::SendToRemoteSubscribers(const ProtoMsg &_msg) const
{
  std::string data;
  if (!_msg.SerializeToString(&data))
  {
    std::cerr << "Node::Publisher::SendToRemoteSubscribers(): "
              << "Error serializing data" << std::endl;
    return false;
  }

  return this->dataPtr->shared->Publish(
    this->dataPtr->publisher.Topic(), data, _msg.GetTypeName());
}

//////////////////////////////////////////////////
bool Node::Publisher::PrePublish(
    const ProtoMsg &_msg, bool &_hasLocalSubscribers,
    std::map<std::string, ISubscriptionHandler_M> &_handlers,
    bool &_hasRemoteSubscribers, bool &_publishResult)
{
  // Sanity check.
  if (!this->Precheck(_msg))
  {
    _publishResult = false;
    return false;
  }

  // Check the publication throttling option.
  if (!this->UpdateThrottling())
  {
    _publishResult = true;
    return false;
  }

  if (!this->CheckSubscribers(_msg, _hasLocalSubscribers, _handlers,
         _hasRemoteSubscribers))
  {
    // No subscribers at all, nothing to do.
    _publishResult = true;
    return false;
  }

  return true;
}

//////////////////////////////////////////////////
bool Node::Publisher::Publish(const ProtoMsg &_msg)
{
  std::map<std::string, ISubscriptionHandler_M> handlers;
  bool hasLocalSubscribers;
  bool hasRemoteSubscribers;
  bool res;
  if (!this->PrePublish(_msg, hasLocalSubscribers, handlers,
         hasRemoteSubscribers, res))
  {
    return res;
  }

  // We need to create a copy of the original message because the message is
  // going to be published in a separate thread. If we don't have exclusive
  // ownership of the message, the caller might destroy or modify the message
  // while it's being published in the separate thread.
  std::shared_ptr<ProtoMsg> msgCopy(_msg.New());
  msgCopy->CopyFrom(_msg);

  this->dataPtr->shared->dataPtr->workerPool.AddWork(
    [handlers, msgCopy, hasLocalSubscribers, hasRemoteSubscribers, this] ()
    {
      if (hasLocalSubscribers)
        this->SendToLocalSubscribers(handlers, *msgCopy);

      if (hasRemoteSubscribers)
        this->SendToRemoteSubscribers(*msgCopy);
    });

  return true;
}

//////////////////////////////////////////////////
bool Node::Publisher::Publish(std::unique_ptr<ProtoMsg> _msg,
  std::function<void(std::unique_ptr<ProtoMsg> _msg, const bool _result)> &_cb)
{
  std::map<std::string, ISubscriptionHandler_M> handlers;
  bool hasLocalSubscribers;
  bool hasRemoteSubscribers;
  bool res;
  if (!this->PrePublish(*_msg, hasLocalSubscribers, handlers,
         hasRemoteSubscribers, res))
  {
    // Notify the caller and transfer back the ownership of the message..
    if (_cb)
      _cb(std::move(_msg), res);
    return res;
  }

  // We transfer the ownership of the original message because the message is
  // going to be published in a separate thread. If we don't have exclusive
  // ownership of the message, the caller might destroy or modify the message
  // while it's being published in the separate thread. When we are done with
  // the message, we'll notify the caller and transfer back the ownership.
  auto t = std::thread(
    [handlers, _cb, msg = std::move(_msg), hasLocalSubscribers,
     hasRemoteSubscribers, this] () mutable
    {
      if (hasLocalSubscribers)
        this->SendToLocalSubscribers(handlers, *msg);

      bool result = true;
      if (hasRemoteSubscribers)
        result = this->SendToRemoteSubscribers(*msg);

      // Notify the caller and transfer back the ownership of the message..
      if (_cb)
        _cb(std::move(msg), result);
    });
  t.detach();

  return true;
}

//////////////////////////////////////////////////
bool Node::Publisher::Publish(
    std::unique_ptr<ProtoMsg> _msg,
    void(*_cb)(std::unique_ptr<ProtoMsg> _msg, const bool _result))
{
  std::function<void(std::unique_ptr<ProtoMsg>, const bool)> f =
    [_cb](std::unique_ptr<ProtoMsg> _internalMsg,
         const bool _internalResult)
    {
      (*_cb)(std::move(_internalMsg), _internalResult);
    };

    return this->Publish(std::move(_msg), f);
}

//////////////////////////////////////////////////
template<typename C>
bool Node::Publisher::Publish(
    std::unique_ptr<ProtoMsg> _msg,
    void(C::*_cb)(std::unique_ptr<ProtoMsg> _msg, const bool _result),
    C *_obj)
{
  std::function<void(std::unique_ptr<ProtoMsg>, const bool)> f =
    [_cb, _obj](std::unique_ptr<ProtoMsg> _internalMsg,
                const bool _internalResult)
    {
      auto cb = std::bind(_cb, _obj, std::placeholders::_1,
        std::placeholders::_2);
      cb(std::move(_internalMsg), _internalResult);
    };

    return this->Publish(std::move(_msg), f);
}

//////////////////////////////////////////////////
bool Node::Publisher::UpdateThrottling()
{
  std::lock_guard<std::mutex> lk(this->dataPtr->mutex);
  if (!this->dataPtr->publisher.Options().Throttled())
    return true;

  Timestamp now = std::chrono::steady_clock::now();

  // Elapsed time since the last callback execution.
  auto elapsed = now - this->dataPtr->lastCbTimestamp;
  if (std::chrono::duration_cast<std::chrono::nanoseconds>(
        elapsed).count() < this->dataPtr->periodNs)
  {
    return false;
  }

  // Update the last callback execution.
  this->dataPtr->lastCbTimestamp = now;
  return true;
}

//////////////////////////////////////////////////
Node::Node(const NodeOptions &_options)
  : dataPtr(new NodePrivate())
{
  // Generate the node UUID.
  Uuid uuid;
  this->dataPtr->nUuid = uuid.ToString();

  // Save the options.
  this->dataPtr->options = _options;
}

//////////////////////////////////////////////////
Node::~Node()
{
  // Unsubscribe from all the topics.
  auto subsTopics = this->SubscribedTopics();
  for (auto const &topic : subsTopics)
    this->Unsubscribe(topic);

  // The list of subscribed topics should be empty.
  assert(this->SubscribedTopics().empty());

  // The list of advertised topics should be empty.
  assert(this->AdvertisedTopics().empty());

  // Unadvertise all my services.
  auto advServices = this->AdvertisedServices();
  for (auto const &service : advServices)
  {
    if (!this->UnadvertiseSrv(service))
    {
      std::cerr << "Node::~Node(): Error unadvertising service ["
                << service << "]" << std::endl;
    }
  }

  // The list of advertised services should be empty.
  assert(this->AdvertisedServices().empty());
}

//////////////////////////////////////////////////
std::vector<std::string> Node::AdvertisedTopics() const
{
  std::vector<std::string> v;
  std::unordered_set<std::string> result;
  std::vector<MessagePublisher> pubs;

  {
    std::lock_guard<std::recursive_mutex> lk(this->dataPtr->shared->mutex);

    auto pUUID = this->dataPtr->shared->pUuid;
    auto &info = this->dataPtr->shared->dataPtr->msgDiscovery->Info();
    info.PublishersByNode(pUUID, this->NodeUuid(), pubs);
  }

  // Copy the topics to a std::set for removing duplications.
  for (auto const &pub : pubs)
    result.insert(pub.Topic());

  // Remove the partition information and convert to std::vector.
  for (auto topic : result)
  {
    topic.erase(0, topic.find_last_of("@") + 1);
    v.push_back(topic);
  }

  return v;
}

//////////////////////////////////////////////////
std::vector<std::string> Node::SubscribedTopics() const
{
  std::vector<std::string> v;

  std::lock_guard<std::recursive_mutex> lk(this->dataPtr->shared->mutex);

  // I'm a real subscriber if I have interest in a topic and I know a publisher.
  for (auto topic : this->dataPtr->topicsSubscribed)
  {
    // Remove the partition information from the topic.
    topic.erase(0, topic.find_last_of("@") + 1);
    v.push_back(topic);
  }

  return v;
}

//////////////////////////////////////////////////
bool Node::Unsubscribe(const std::string &_topic)
{
  std::string fullyQualifiedTopic;
  if (!TopicUtils::FullyQualifiedName(this->Options().Partition(),
    this->Options().NameSpace(), _topic, fullyQualifiedTopic))
  {
    std::cerr << "Topic [" << _topic << "] is not valid." << std::endl;
    return false;
  }

  std::lock_guard<std::recursive_mutex> lk(this->dataPtr->shared->mutex);

  this->dataPtr->shared->localSubscriptions.RemoveHandlersForNode(
    fullyQualifiedTopic, this->dataPtr->nUuid);

  // Remove the topic from the list of subscribed topics in this node.
  this->dataPtr->topicsSubscribed.erase(fullyQualifiedTopic);

  // Remove the filter for this topic if I am the last subscriber.
  if (!this->dataPtr->shared->localSubscriptions.HasHandlersForTopic(
    fullyQualifiedTopic))
  {
    this->dataPtr->shared->dataPtr->subscriber->setsockopt(
      ZMQ_UNSUBSCRIBE, fullyQualifiedTopic.data(), fullyQualifiedTopic.size());
  }

  // Notify to the publishers that I am no longer interested in the topic.
  MsgAddresses_M addresses;
  if (!this->dataPtr->shared->dataPtr->msgDiscovery->Publishers(
        fullyQualifiedTopic,
    addresses))
  {
    return false;
  }

  for (auto &proc : addresses)
  {
    for (auto &node : proc.second)
    {
      zmq::socket_t socket(*this->dataPtr->shared->dataPtr->context,
          ZMQ_DEALER);

      // Set ZMQ_LINGER to 0 means no linger period. Pending messages will be
      // discarded immediately when the socket is closed. That avoids infinite
      // waits if the publisher is disconnected.
      int lingerVal = 200;
      socket.setsockopt(ZMQ_LINGER, &lingerVal, sizeof(lingerVal));

      socket.connect(node.Ctrl().c_str());

      zmq::message_t msg;
      msg.rebuild(fullyQualifiedTopic.size());
      memcpy(msg.data(), fullyQualifiedTopic.data(),
        fullyQualifiedTopic.size());
      socket.send(msg, ZMQ_SNDMORE);

      msg.rebuild(this->dataPtr->shared->myAddress.size());
      memcpy(msg.data(), this->dataPtr->shared->myAddress.data(),
             this->dataPtr->shared->myAddress.size());
      socket.send(msg, ZMQ_SNDMORE);

      msg.rebuild(this->dataPtr->nUuid.size());
      memcpy(msg.data(), this->dataPtr->nUuid.data(),
             this->dataPtr->nUuid.size());
      socket.send(msg, ZMQ_SNDMORE);

      msg.rebuild(kGenericMessageType.size());
      memcpy(msg.data(), kGenericMessageType.data(),
             kGenericMessageType.size());
      socket.send(msg, ZMQ_SNDMORE);

      std::string data = std::to_string(EndConnection);
      msg.rebuild(data.size());
      memcpy(msg.data(), data.data(), data.size());
      socket.send(msg, 0);
    }
  }

  return true;
}

//////////////////////////////////////////////////
std::vector<std::string> Node::AdvertisedServices() const
{
  std::vector<std::string> v;

  std::lock_guard<std::recursive_mutex> lk(this->dataPtr->shared->mutex);

  for (auto service : this->dataPtr->srvsAdvertised)
  {
    // Remove the partition information from the service name.
    service.erase(0, service.find_last_of("@") + 1);
    v.push_back(service);
  }

  return v;
}

//////////////////////////////////////////////////
bool Node::UnadvertiseSrv(const std::string &_topic)
{
  std::string fullyQualifiedTopic;
  if (!TopicUtils::FullyQualifiedName(this->Options().Partition(),
    this->Options().NameSpace(), _topic, fullyQualifiedTopic))
  {
    std::cerr << "Service [" << _topic << "] is not valid." << std::endl;
    return false;
  }

  std::lock_guard<std::recursive_mutex> lk(this->dataPtr->shared->mutex);

  // Remove the topic from the list of advertised topics in this node.
  this->dataPtr->srvsAdvertised.erase(fullyQualifiedTopic);

  // Remove all the REP handlers for this node.
  this->dataPtr->shared->repliers.RemoveHandlersForNode(
    fullyQualifiedTopic, this->dataPtr->nUuid);

  // Notify the discovery service to unregister and unadvertise my services.
  if (!this->dataPtr->shared->dataPtr->srvDiscovery->Unadvertise(
        fullyQualifiedTopic, this->dataPtr->nUuid))
  {
    return false;
  }

  return true;
}

//////////////////////////////////////////////////
void Node::TopicList(std::vector<std::string> &_topics) const
{
  std::vector<std::string> allTopics;
  _topics.clear();

  this->dataPtr->shared->dataPtr->msgDiscovery->TopicList(allTopics);

  for (auto &topic : allTopics)
  {
    // Get the partition name.
    std::string partition = topic.substr(1, topic.find_last_of("@") - 1);
    // Remove the front '/'
    if (!partition.empty())
      partition.erase(partition.begin());

    // Discard if the partition name does not match this node's partition.
    if (partition != this->Options().Partition())
      continue;

    // Remove the partition part from the topic.
    topic.erase(0, topic.find_last_of("@") + 1);

    _topics.push_back(topic);
  }
}

//////////////////////////////////////////////////
void Node::ServiceList(std::vector<std::string> &_services) const
{
  std::vector<std::string> allServices;
  _services.clear();

  this->dataPtr->shared->dataPtr->srvDiscovery->TopicList(allServices);

  for (auto &service : allServices)
  {
    // Get the partition name.
    std::string partition = service.substr(1, service.find_last_of("@") - 1);
    // Remove the front '/'
    if (!partition.empty())
      partition.erase(partition.begin());

    // Discard if the partition name does not match this node's partition.
    if (partition != this->Options().Partition())
      continue;

    // Remove the partition part from the service.
    service.erase(0, service.find_last_of("@") + 1);

    _services.push_back(service);
  }
}

//////////////////////////////////////////////////
NodeShared *Node::Shared() const
{
  return this->dataPtr->shared;
}

//////////////////////////////////////////////////
const std::string &Node::NodeUuid() const
{
  return this->dataPtr->nUuid;
}

//////////////////////////////////////////////////
std::unordered_set<std::string> &Node::TopicsSubscribed() const
{
  return this->dataPtr->topicsSubscribed;
}

//////////////////////////////////////////////////
std::unordered_set<std::string> &Node::SrvsAdvertised() const
{
  return this->dataPtr->srvsAdvertised;
}

//////////////////////////////////////////////////
NodeOptions &Node::Options() const
{
  return this->dataPtr->options;
}

//////////////////////////////////////////////////
bool Node::TopicInfo(const std::string &_topic,
                     std::vector<MessagePublisher> &_publishers) const
{
  this->dataPtr->shared->dataPtr->msgDiscovery->WaitForInit();

  // Construct a topic name with the partition and namespace
  std::string fullyQualifiedTopic;
  if (!TopicUtils::FullyQualifiedName(this->Options().Partition(),
    this->Options().NameSpace(), _topic, fullyQualifiedTopic))
  {
    return false;
  }

  std::lock_guard<std::recursive_mutex> lk(this->dataPtr->shared->mutex);

  // Get all the publishers on the given topics
  MsgAddresses_M pubs;
  if (!this->dataPtr->shared->dataPtr->msgDiscovery->Publishers(
        fullyQualifiedTopic, pubs))
  {
    return false;
  }

  _publishers.clear();

  // Copy the publishers.
  for (MsgAddresses_M::iterator iter = pubs.begin(); iter != pubs.end(); ++iter)
  {
    for (std::vector<MessagePublisher>::iterator pubIter = iter->second.begin();
         pubIter != iter->second.end(); ++pubIter)
    {
      // Add the publisher if it doesn't already exist.
      if (std::find(_publishers.begin(), _publishers.end(), *pubIter) ==
          _publishers.end())
      {
        _publishers.push_back(*pubIter);
      }
    }
  }

  return true;
}

//////////////////////////////////////////////////
bool Node::ServiceInfo(const std::string &_service,
                       std::vector<ServicePublisher> &_publishers) const
{
  this->dataPtr->shared->dataPtr->srvDiscovery->WaitForInit();

  // Construct a topic name with the partition and namespace
  std::string fullyQualifiedTopic;
  if (!TopicUtils::FullyQualifiedName(this->Options().Partition(),
    this->Options().NameSpace(), _service, fullyQualifiedTopic))
  {
    return false;
  }

  std::lock_guard<std::recursive_mutex> lk(this->dataPtr->shared->mutex);

  // Get all the publishers on the given service.
  SrvAddresses_M pubs;
  if (!this->dataPtr->shared->dataPtr->srvDiscovery->Publishers(
        fullyQualifiedTopic, pubs))
  {
    return false;
  }

  _publishers.clear();

  // Copy the publishers.
  for (SrvAddresses_M::iterator iter = pubs.begin(); iter != pubs.end(); ++iter)
  {
    for (std::vector<ServicePublisher>::iterator pubIter = iter->second.begin();
         pubIter != iter->second.end(); ++pubIter)
    {
      // Add the publisher if it doesn't already exist.
      if (std::find(_publishers.begin(), _publishers.end(), *pubIter) ==
          _publishers.end())
      {
        _publishers.push_back(*pubIter);
      }
    }
  }

  return true;
}

/////////////////////////////////////////////////
Node::Publisher Node::Advertise(const std::string &_topic,
    const std::string &_msgTypeName, const AdvertiseMessageOptions &_options)
{
  std::string fullyQualifiedTopic;
  if (!TopicUtils::FullyQualifiedName(this->Options().Partition(),
        this->Options().NameSpace(), _topic, fullyQualifiedTopic))
  {
    std::cerr << "Topic [" << _topic << "] is not valid." << std::endl;
    return Publisher();
  }

  auto currentTopics = this->AdvertisedTopics();

  if (std::find(currentTopics.begin(), currentTopics.end(),
        fullyQualifiedTopic) != currentTopics.end())
  {
    std::cerr << "Topic [" << _topic << "] already advertised. You cannot"
      << " advertise the same topic twice on the same node."
      << " If you want to advertise the same topic with different"
      << " types, use separate nodes" << std::endl;
    return Publisher();
  }

  std::lock_guard<std::recursive_mutex> lk(this->Shared()->mutex);

  // Notify the discovery service to register and advertise my topic.
  MessagePublisher publisher(fullyQualifiedTopic,
      this->Shared()->myAddress,
      this->Shared()->myControlAddress,
      this->Shared()->pUuid, this->NodeUuid(), _msgTypeName, _options);

  if (!this->Shared()->dataPtr->msgDiscovery->Advertise(publisher))
  {
    std::cerr << "Node::Advertise(): Error advertising a topic. "
      << "Did you forget to start the discovery service?"
      << std::endl;
    return Publisher();
  }

  return Publisher(publisher);
}

/////////////////////////////////////////////////
bool Node::SubscribeHelper(const std::string &_fullyQualifiedTopic)
{
  // Add the topic to the list of subscribed topics (if it was not before).
  this->TopicsSubscribed().insert(_fullyQualifiedTopic);

  // Discover the list of nodes that publish on the topic.
  if (!this->Shared()->dataPtr->msgDiscovery->Discover(_fullyQualifiedTopic))
  {
    std::cerr << "Node::Subscribe(): Error discovering a topic. "
              << "Did you forget to start the discovery service?"
              << std::endl;
    return false;
  }

  return true;
}

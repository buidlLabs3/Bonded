#pragma once

#include "integrations/interfaces.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>

namespace bonded {

class LogosMessagingAdapter : public MessagingAdapter {
public:
    using Send = std::function<std::string(const std::string&, const std::string&)>;
    using Subscribe = std::function<void(const std::string&)>;

    LogosMessagingAdapter(Send send, Subscribe subscribe);

    std::string send(const std::string& topic, const std::string& payload) override;
    void subscribe(const std::string& topic,
                   std::function<void(const std::string&)> handler) override;
    void join(const std::string& group_id) override;
    std::string createGroup(const std::vector<std::string>& members) override;

    void receive(const std::string& topic, const std::string& payload);

private:
    struct Subscriber {
        std::uint64_t id;
        std::function<void(const std::string&)> handler;
    };

    static std::string groupTopic(const std::string& group_id);
    void ensureSubscribed(const std::string& topic,
                          std::function<void(const std::string&)> handler = {});

    Send send_;
    Subscribe subscribe_;
    std::mutex mutex_;
    std::condition_variable subscription_changed_;
    std::map<std::string, std::vector<Subscriber>> subscribers_;
    std::set<std::string> subscribed_topics_;
    std::set<std::string> subscribing_topics_;
    std::uint64_t next_subscriber_id_{0};
};

class LogosStorageAdapter : public StorageAdapter {
public:
    using Upload = std::function<std::string(const std::string&)>;
    using Download = std::function<std::string(const std::string&)>;
    using Cancel = std::function<void(const std::string&)>;
    using Cleanup = std::function<void(const std::string&)>;

    LogosStorageAdapter(Upload upload, Download download, Cancel cancel_upload,
                        Cancel cancel_download, Cleanup cleanup_upload,
                        std::chrono::milliseconds timeout);

    std::string put(const std::string& payload) override;
    std::string get(const std::string& address) const override;

    void uploadDone(const std::string& event_json);
    void downloadProgress(const std::string& event_json);
    void downloadDone(const std::string& event_json);

private:
    struct PendingUpload {
        bool done{false};
        bool success{false};
        std::string cid;
        std::string error;
    };

    struct PendingDownload {
        bool done{false};
        bool success{false};
        std::string payload;
        std::string error;
    };

    Upload upload_;
    Download download_;
    Cancel cancel_upload_;
    Cancel cancel_download_;
    Cleanup cleanup_upload_;
    std::chrono::milliseconds timeout_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    mutable std::map<std::string, PendingUpload> uploads_;
    mutable std::map<std::string, PendingDownload> downloads_;
    mutable std::set<std::string> waiting_uploads_;
    mutable std::set<std::string> waiting_downloads_;
    mutable std::set<std::string> expired_uploads_;
    mutable std::set<std::string> expired_downloads_;
    mutable std::size_t starting_uploads_{0};
    mutable std::size_t starting_downloads_{0};
};

} // namespace bonded

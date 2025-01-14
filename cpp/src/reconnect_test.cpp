/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "test_bits.hpp"
#include "proton/connection.hpp"
#include "proton/connection_options.hpp"
#include "proton/container.hpp"
#include "proton/delivery.hpp"
#include "proton/error_condition.hpp"
#include "proton/listen_handler.hpp"
#include "proton/listener.hpp"
#include "proton/message.hpp"
#include "proton/messaging_handler.hpp"
#include "proton/reconnect_options.hpp"
#include "proton/receiver_options.hpp"
#include "proton/transport.hpp"
#include "proton/work_queue.hpp"

#include <cstdlib>
#include <ctime>
#include <string>
#include <cstdio>
#include <memory>
#include <sstream>

namespace {

// Wait for N things to be done.
class waiter {
    size_t count;
  public:
    waiter(size_t n) : count(n) {}
    void done() { if (--count == 0) ready(); }
    virtual void ready() = 0;
};

class server_connection_handler : public proton::messaging_handler {

    struct listen_handler : public proton::listen_handler {
        proton::connection_options opts;
        std::string url;
        waiter& listen_waiter;

        listen_handler(proton::messaging_handler& h, waiter& w) : listen_waiter(w) {
            opts.handler(h);
        }

        void on_open(proton::listener& l) override {
            std::ostringstream o;
            o << "//:" << l.port(); // Connect to the actual listening port
            url = o.str();
            // Schedule rather than call done() direct to ensure serialization
            l.container().schedule(proton::duration::IMMEDIATE,
                                   proton::make_work(&waiter::done, &listen_waiter));
        }

        proton::connection_options on_accept(proton::listener&) override { return opts; }
    };

    listen_handler listen_handler_;
    proton::listener listener_;
    int messages_ = 0;
    int expect_;
    bool closing_ = false;

    void close (proton::connection &c) {
        if (closing_) return;

        c.close(proton::error_condition("amqp:connection:forced", "Failover testing"));
        closing_ = true;
    }

  public:
    server_connection_handler(proton::container& c, int e, waiter& w)
        : listen_handler_(*this, w), expect_(e)
    {
        listener_ = c.listen("//:0", listen_handler_);
    }

    std::string url() const {
        if (listen_handler_.url.empty()) throw std::runtime_error("no url");
        return listen_handler_.url;
    }

    void on_connection_open(proton::connection &c) override {
        // Only listen for a single connection
        listener_.stop();
        if (messages_==expect_) close(c);
        else c.open();
    }

    void on_receiver_open(proton::receiver &r) override {
        // Reduce message noise in PN_TRACE output for debugging.
        // Only the first message is relevant
        // Control accepts, accepting the message tells the client to finally close.
        r.open(proton::receiver_options().credit_window(0).auto_accept(false));
        r.add_credit(1);
    }

    void on_message(proton::delivery & d, proton::message & m) override {
        ++messages_;
        proton::connection c = d.connection();
        if (messages_==expect_) close(c);
        else d.accept();
    }

    void on_transport_error(proton::transport & ) override {
        // If we get an error then (try to) stop the listener
        // - this will stop the listener if we didn't already accept a connection
        listener_.stop();
    }
};

class tester_base: public proton::messaging_handler {
  void on_connection_open(proton::connection& c) override {
    if (!c.reconnected()) {
      start_count++;
      c.open_sender("messages");
    }
    ASSERT_EQUAL(bool(open_count), c.reconnected());
    open_count++;
  }

  void on_connection_error(proton::connection& c) override {
    connection_error_count++;
  }

  void on_sender_open(proton::sender &s) override {
    link_open_count++;
  }

  void on_sendable(proton::sender& s) override {
    s.send(proton::message("hello"));
  }

  void on_tracker_accept(proton::tracker& d) override {
    d.connection().close();
  }

  void on_transport_error(proton::transport& t) override {
    ASSERT_EQUAL(bool(transport_error_count), t.connection().reconnected());
    transport_error_count++;
  }

  void on_transport_close(proton::transport& t) override {
    transport_close_count++;
  }

protected:
  int start_count = 0;
  int open_count = 0;
  int link_open_count = 0;
  int transport_error_count = 0;
  int transport_close_count = 0;
  int connection_error_count = 0;
};

class tester : public tester_base, public waiter {
  public:
    tester() : waiter(3), container_(*this, "reconnect_client") {}

    void on_container_start(proton::container &c) override {
        // Server that fails upon connection
        s1.reset(new server_connection_handler(c, 0, *this));
        // Server that fails on first message
        s2.reset(new server_connection_handler(c, 1, *this));
        // server that doesn't fail in this test
        s3.reset(new server_connection_handler(c, 100, *this));
    }

    // waiter::ready is called when all 3 listeners are ready.
    void ready() override {
        container_.connect(s1->url(), proton::connection_options().failover_urls({s2->url(), s3->url()}));
    }

    void run() {
        container_.run();
        ASSERT_EQUAL(1, start_count);
        ASSERT_EQUAL(3, open_count);
        // Could be > 3, unpredictable number reconnects while listener comes up.
        ASSERT(2 < transport_error_count);
        // Last reconnect fails before opening links
        ASSERT(1 < link_open_count);
        // One final transport close, not an error
        ASSERT_EQUAL(1, transport_close_count);
        ASSERT_EQUAL(0, connection_error_count);
    }

  private:
    std::unique_ptr<server_connection_handler> s1;
    std::unique_ptr<server_connection_handler> s2;
    std::unique_ptr<server_connection_handler> s3;
    proton::container container_;
};

int test_failover_simple() {
    tester().run();
    return 0;
}

class empty_failover_tester : public tester_base, public waiter {
  public:
     empty_failover_tester() : waiter(1), container_(*this, "reconnect_client") {}

    void on_container_start(proton::container &c) override {
        // Server that fails upon connection
        s1.reset(new server_connection_handler(c, 0, *this));
    }

    // waiter::ready is called when a listener is ready.
    void ready() override {
        container_.connect(s1->url(), proton::connection_options().failover_urls({}));
    }

    void run() {
        container_.run();
        ASSERT_EQUAL(1, start_count);
        ASSERT_EQUAL(1, open_count);
        // Could be >=0, unpredictable number reconnects while listener comes up.
        ASSERT(0 <= transport_error_count);
        ASSERT(0 <= link_open_count);
        ASSERT_EQUAL(1, transport_close_count);
        ASSERT_EQUAL(1, connection_error_count);
    }

  private:
    std::unique_ptr<server_connection_handler> s1;
    proton::container container_;
};

int test_empty_failover() {
    empty_failover_tester().run();
    return 0;
}

}

class stop_reconnect_tester : public proton::messaging_handler {
  public:
    stop_reconnect_tester() :
        container_(*this, "reconnect_tester")
    {
    }

    void deferred_stop() {
        container_.stop();
    }

    void on_container_start(proton::container &c) override {
        proton::reconnect_options reconnect_options;
        c.connect("this-is-not-going-to work.com", proton::connection_options().reconnect(reconnect_options));
        c.schedule(proton::duration::SECOND, proton::make_work(&stop_reconnect_tester::deferred_stop, this));
    }

    void run() {
        container_.run();
    }

  private:
    proton::container container_;
};

int test_stop_reconnect() {
    stop_reconnect_tester().run();
    return 0;
}

class authfail_reconnect_tester : public proton::messaging_handler, public waiter {
  public:
    authfail_reconnect_tester() :
        waiter(1), container_(*this, "authfail_reconnect_tester")
    {}

    void deferred_stop() {
        container_.stop();
    }

    void on_container_start(proton::container& c) override {
        // This server won't fail in this test
        s1.reset(new server_connection_handler(c, 100, *this));
        c.schedule(proton::duration::SECOND, proton::make_work(&authfail_reconnect_tester::deferred_stop, this));
    }

    void on_transport_error(proton::transport& t) override {
        errored_ = true;
    }

    void ready() override {
        proton::connection_options co;
        co.sasl_allowed_mechs("PLAIN");
        co.reconnect(proton::reconnect_options());
        container_.connect(s1->url(), co);
    }

    void run() {
        container_.run();
        ASSERT(errored_);
    }

  private:
    proton::container container_;
    std::unique_ptr<server_connection_handler> s1;
    bool errored_ = false;
};

// Verify we can stop reconnecting by calling close() in on_transport_error()
class test_reconnecting_close : public proton::messaging_handler, public waiter {
  public:
    test_reconnecting_close() : waiter(1), container_(*this, "test_reconnecting_close") {}

    void on_container_start(proton::container &c) override {
        s1.reset(new server_connection_handler(c, 0, *this));
    }

    void ready() override {
        container_.connect(s1->url(), proton::connection_options().reconnect(proton::reconnect_options()));
    }

    void on_transport_error(proton::transport& t) override {
        transport_error_called = true;
        t.connection().close();                        // Abort reconnection
    }

    void on_connection_close(proton::connection& c) override {
        ASSERT(0);              // Not expecting any clean close
    }

    void run() {
        container_.run();
    }

  private:
    proton::container container_;
    std::string err_;
    bool transport_error_called = false;
    std::unique_ptr<server_connection_handler> s1;
};

int test_auth_fail_reconnect() {
    authfail_reconnect_tester().run();
    return 0;
}

class test_reconnect_url : public proton::messaging_handler {
public:
    test_reconnect_url()
            : container_(*this, "test_reconnect_update") {}

    proton::reconnect_options ropts() {
        // Fast as we can to avoid needless test slowness.
        return proton::reconnect_options().delay(proton::duration::MILLISECOND);
    }

    proton::connection_options copts() { return proton::connection_options(); }

    void on_container_start(proton::container &c) override {
        // Never actually connects, keeps re-trying to bogus hostnames with
        // changing options.
        c.connect("nosuchhost0",
                  copts()
                  .reconnect(ropts())
                  .virtual_host("vhost0")
                  .user("user0")
                  .reconnect_url("hahaha1"));
    }

    void on_transport_error(proton::transport &t) override {
        switch (++errors_) {
        case 1:
            ASSERT_SUBSTRING("nosuchhost0", t.error().what()); // First failure
            break;
        case 2: {
            ASSERT_SUBSTRING("hahaha1",t.error().what()); // Second failure
            ASSERT_EQUAL("user0", t.connection().user());
            break;
        }
        case 3:
            ASSERT_SUBSTRING("hahaha1", t.error().what()); // Still trying reconnect url
            t.connection().update_options(copts().reconnect_url("nosuchhost1"));
            // Verify changing reconnect options does not affect other options.
            ASSERT_EQUAL("user0", t.connection().user());
            break;
        case 4:
            ASSERT_SUBSTRING("nosuchhost1", t.error().what()); // Re-try new reconnect url
            break;
        default:
            t.connection().container().stop();
        }
    }

    void run() { container_.run(); }

private:
    int errors_ = 0;
    proton::container container_;
};

// Verify we can change connection options for reconnect on_transport_error()
class test_reconnect_update_failover : public proton::messaging_handler {
public:
    test_reconnect_update_failover()
            : container_(*this, "test_reconnect_update") {}

    proton::reconnect_options ropts() {
        // Fast as we can to avoid needless test slowness.
        return proton::reconnect_options().delay(proton::duration::MILLISECOND);
    }

    proton::connection_options copts() { return proton::connection_options(); }

    void on_container_start(proton::container &c) override {
        // Never actually connects, keeps re-trying to bogus hostnames with
        // changing options.
        c.connect("nosuchhost0", copts().reconnect(ropts()).virtual_host("vhost0").user("user0"));
    }

    void on_transport_error(proton::transport &t) override {
        switch (++errors_) {
        case 1:
            ASSERT_SUBSTRING("nosuchhost0", t.error().what()); // First failure
            break;
        case 2: {
            ASSERT_SUBSTRING("nosuchhost0",t.error().what()); // Second failure
            std::vector<std::string> urls;
            urls.push_back("nosuchhost1");
            // Update the failover list
            t.connection().update_options(copts().failover_urls(urls));
            // Verify changing reconnect options does not affect other options.
            ASSERT_EQUAL("user0", t.connection().user());
            break;
        }
        case 3:
            ASSERT_SUBSTRING("nosuchhost1", t.error().what()); // Using failover host
            // Change a non-reconnect option should not affect reconnect
            t.connection().update_options(copts().user("user1"));
            break;
        case 4:
            ASSERT_SUBSTRING("nosuchhost0", t.error().what()); // Back to original url
            ASSERT_EQUAL("user1", t.connection().user());
            break;
        case 5:
            ASSERT_SUBSTRING("nosuchhost1", t.error().what()); // Still have failover
            break;
        default:
            t.connection().container().stop();
        }
    }

    void run() { container_.run(); }

private:
    int errors_ = 0;
    proton::container container_;
};

class test_reconnect_update_simple : public proton::messaging_handler {
public:
    test_reconnect_update_simple()
            : container_(*this, "test_reconnect_update") {}

    proton::reconnect_options ropts() {
        // Fast as we can to avoid needless test slowness.
        return proton::reconnect_options().delay(proton::duration::MILLISECOND);
    }

    proton::connection_options copts() { return proton::connection_options(); }

    void on_container_start(proton::container &c) override {
        // Never actually connects, keeps re-trying to bogus hostnames with
        // changing options.
        c.connect("nosuchhost0", copts().reconnect(ropts()).virtual_host("vhost0").user("user0"));
    }

    void on_transport_error(proton::transport &t) override {
        switch (++errors_) {
        case 1:
            ASSERT_SUBSTRING("nosuchhost0", t.error().what()); // First failure
            break;
        case 2: {
            ASSERT_SUBSTRING("nosuchhost0",t.error().what()); // Second failure
            t.connection().update_options(copts().reconnect_url("nosuchhost1"));
            // Verify changing reconnect options does not affect other options.
            ASSERT_EQUAL("user0", t.connection().user());
            break;
        }
        case 3:
            ASSERT_SUBSTRING("nosuchhost1", t.error().what()); // Re-try original
            t.connection().update_options(copts().reconnect_url("notsuchahostatall"));
            break;
        case 4:
            ASSERT_SUBSTRING("notsuchahostatall", t.error().what()); // Re-try new reconnect url
            break;
        case 5:
            ASSERT_SUBSTRING("notsuchahostatall", t.error().what()); // Re-try new reconnect url
            // Change a non-reconnect option should not affect reconnect
            t.connection().update_options(copts().user("user1"));
            break;
        case 6:
            ASSERT_SUBSTRING("notsuchahostatall", t.error().what()); // Same reconnect url
            ASSERT_EQUAL("user1", t.connection().user());
            t.connection().update_options(copts().reconnect_url("nosuchhost1"));
            break;
        case 7:
            ASSERT_SUBSTRING("nosuchhost1", t.error().what());
            break;
        default:
            t.connection().container().stop();
        }
    }

    void run() { container_.run(); }

private:
    int errors_ = 0;
    proton::container container_;
};

int main(int argc, char **argv) {
    int failed = 0;
    RUN_ARGV_TEST(failed, test_failover_simple());
    RUN_ARGV_TEST(failed, test_empty_failover());
    RUN_ARGV_TEST(failed, test_stop_reconnect());
    RUN_ARGV_TEST(failed, test_auth_fail_reconnect());
    RUN_ARGV_TEST(failed, test_reconnecting_close().run());
    RUN_ARGV_TEST(failed, test_reconnect_url().run());
    RUN_ARGV_TEST(failed, test_reconnect_update_failover().run());
    RUN_ARGV_TEST(failed, test_reconnect_update_simple().run());
    return failed;
}

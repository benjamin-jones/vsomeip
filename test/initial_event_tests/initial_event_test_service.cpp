// Copyright (C) 2014-2016 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <map>
#include <algorithm>

#include <gtest/gtest.h>

#include <vsomeip/vsomeip.hpp>
#include "../../implementation/logging/include/logger.hpp"

#include "initial_event_test_globals.hpp"


class initial_event_test_service {
public:
    initial_event_test_service(struct initial_event_test::service_info _service_info) :
            service_info_(_service_info),
            app_(vsomeip::runtime::get()->create_application()),
            wait_until_registered_(true),
            offer_thread_(std::bind(&initial_event_test_service::run, this)) {
        if (!app_->init()) {
            ADD_FAILURE() << "Couldn't initialize application";
            return;
        }
        app_->register_state_handler(
                std::bind(&initial_event_test_service::on_state, this,
                        std::placeholders::_1));

        // offer field
        std::set<vsomeip::eventgroup_t> its_eventgroups;
        its_eventgroups.insert(service_info_.eventgroup_id);
        app_->offer_event(service_info_.service_id, service_info_.instance_id,
                service_info_.event_id, its_eventgroups, true);

        // set value to field
        std::shared_ptr<vsomeip::payload> its_payload =
                vsomeip::runtime::get()->create_payload();
        vsomeip::byte_t its_data[2] = {static_cast<vsomeip::byte_t>((service_info_.service_id & 0xFF00) >> 8),
                static_cast<vsomeip::byte_t>((service_info_.service_id & 0xFF))};
        its_payload->set_data(its_data, 2);
        app_->notify(service_info_.service_id, service_info_.instance_id,
                service_info_.event_id, its_payload);

        app_->start();
    }

    ~initial_event_test_service() {
        offer_thread_.join();
    }

    void offer() {
        app_->offer_service(service_info_.service_id, service_info_.instance_id);
    }

    void on_state(vsomeip::state_type_e _state) {
        VSOMEIP_INFO << "Application " << app_->get_name() << " is "
        << (_state == vsomeip::state_type_e::ST_REGISTERED ?
                "registered." : "deregistered.");

        if (_state == vsomeip::state_type_e::ST_REGISTERED) {
            std::lock_guard<std::mutex> its_lock(mutex_);
            wait_until_registered_ = false;
            condition_.notify_one();
        }
    }

    void run() {
        VSOMEIP_DEBUG << "[" << std::setw(4) << std::setfill('0') << std::hex
                << service_info_.service_id << "] Running";
        std::unique_lock<std::mutex> its_lock(mutex_);
        while (wait_until_registered_) {
            condition_.wait(its_lock);
        }

        VSOMEIP_DEBUG << "[" << std::setw(4) << std::setfill('0') << std::hex
                << service_info_.service_id << "] Offering";
        offer();
    }

private:
    initial_event_test::service_info service_info_;
    std::shared_ptr<vsomeip::application> app_;

    bool wait_until_registered_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread offer_thread_;
};

static int service_number;
static bool use_same_service_id;

TEST(someip_initial_event_test, set_field_once)
{
    if(use_same_service_id) {
        initial_event_test_service its_sample(
                initial_event_test::service_infos_same_service_id[service_number]);
    } else {
        initial_event_test_service its_sample(
                initial_event_test::service_infos[service_number]);
    }
}

#ifndef WIN32
int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    if(argc < 2) {
        std::cerr << "Please specify a service number and subscription type, like: " << argv[0] << " 2 SAME_SERVICE_ID" << std::endl;
        std::cerr << "Valid service numbers are in the range of [1,6]" << std::endl;
        std::cerr << "If SAME_SERVICE_ID is specified as third parameter the test is run w/ multiple instances of the same service" << std::endl;
        return 1;
    }

    service_number = std::stoi(std::string(argv[1]), nullptr);

    if (argc >= 3 && std::string("SAME_SERVICE_ID") == std::string(argv[2])) {
        use_same_service_id = true;
    } else {
        use_same_service_id = false;
    }
    return RUN_ALL_TESTS();
}
#endif

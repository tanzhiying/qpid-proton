#ifndef PROTON_CPP_BLOCKING_RECEIVER_HPP
#define PROTON_CPP_BLOCKING_RECEIVER_HPP

/*
 *
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
 *
 */
#include "proton/export.hpp"
#include "proton/container.hpp"
#include "proton/blocking_link.hpp"
#include "proton/duration.hpp"
#include "proton/messaging_handler.hpp"
#include "proton/types.h"
#include "proton/delivery.h"
#include <string>

namespace proton {

class blocking_connection;
class blocking_link;
class fetcher;

class blocking_receiver : public blocking_link
{
  public:
    PN_CPP_EXTERN blocking_receiver(const blocking_receiver&);
    PN_CPP_EXTERN blocking_receiver& operator=(const blocking_receiver&);
    PN_CPP_EXTERN ~blocking_receiver();
    PN_CPP_EXTERN message receive();
    PN_CPP_EXTERN message receive(duration timeout);
    PN_CPP_EXTERN void accept();
    PN_CPP_EXTERN void reject();
    PN_CPP_EXTERN void release(bool delivered = true);
    PN_CPP_EXTERN void settle();
    PN_CPP_EXTERN void settle(delivery::state state);
    PN_CPP_EXTERN void flow(int count);
    /** Credit available on the receiver link */
    PN_CPP_EXTERN int credit();
    /** Local source of the receiver link */
    PN_CPP_EXTERN terminus source();
    /** Local target of the receiver link */
    PN_CPP_EXTERN terminus target();
    /** Remote source of the receiver link */
    PN_CPP_EXTERN terminus remote_source();
    /** Remote target of the receiver link */
    PN_CPP_EXTERN terminus remote_target();

  private:
    blocking_receiver(blocking_connection &c, receiver &l, fetcher *f, int credit);
    fetcher *fetcher_;
    friend class blocking_connection;
};

}

#endif  /*!PROTON_CPP_BLOCKING_RECEIVER_HPP*/

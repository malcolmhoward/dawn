/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 */

/**
 * @file mosquitto_comms.h
 * @brief MQTT communication layer and device callback routing.
 *
 * Provides MQTT connection callbacks and the device callback lookup
 * interface used by the command router. Device callbacks are registered
 * via the tool_registry system; this header exposes the lookup function
 * and MQTT event handlers.
 */

#ifndef MOSQUITTO_COMMS_H
#define MOSQUITTO_COMMS_H

#include <mosquitto.h>

/* MQTT callbacks */

/**
 * @brief Callback function invoked when the client successfully connects to the MQTT broker.
 *
 * @param mosq        The Mosquitto client instance.
 * @param obj         User-defined pointer passed to the callback.
 * @param reason_code The reason code for the connection result.
 */
void on_connect(struct mosquitto *mosq, void *obj, int reason_code);

/**
 * @brief Callback function invoked when the client successfully subscribes to a topic.
 *
 * @param mosq        The Mosquitto client instance.
 * @param obj         User-defined pointer passed to the callback.
 * @param mid         Message ID of the subscription request.
 * @param qos_count   The number of granted QoS levels.
 * @param granted_qos Array of granted QoS levels.
 */
void on_subscribe(struct mosquitto *mosq,
                  void *obj,
                  int mid,
                  int qos_count,
                  const int *granted_qos);

/**
 * @brief Callback function invoked when a message is received from the subscribed topics.
 *
 * @param mosq The Mosquitto client instance.
 * @param obj  User-defined pointer passed to the callback.
 * @param msg  The message data received.
 */
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);

#endif  // MOSQUITTO_COMMS_H

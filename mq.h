/**
* @file mq.h 
* @author Tsaplay Yuriy (y.tsaplay@yukonww.com)
*
* @brief 
*
*/
#ifndef SUPER_CLOCK_MQ_H
#define SUPER_CLOCK_MQ_H

#include <mosquitto.h>

typedef void (*mosq_cb_t)(const struct mosquitto_message *msg);

void mosq_init(const char *progname, const char *host_name);

void mosq_destroy(void);

void mosq_register_on_message_cb(const char *topic, mosq_cb_t cb);

#endif //SUPER_CLOCK_MQ_H
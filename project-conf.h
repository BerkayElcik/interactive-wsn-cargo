#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

// Force standard CSMA/CA MAC protocol
#define MAC_CONF_WITH_CSMA 1

// Set the radio channel (e.g., 26 for your lab group to avoid interference)
#define IEEE802154_CONF_DEFAULT_CHANNEL 16

// Set NullNet as the network layer
#define NETSTACK_CONF_NETWORK nullnet_driver

#endif /* PROJECT_CONF_H_ */
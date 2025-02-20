#pragma once

#include <Arduino.h>
#if defined ESP32
#include <WiFi.h>
#include <esp_wifi.h>
#elif defined ESP8266
#include <ESP8266WiFi.h>
#define WIFI_MODE_STA WIFI_STA 
#else
#error "Unsupported platform"
#endif //ESP32
#include <QuickEspNow.h>

#include "global_state.h"

// #define NODE_DEBUGGING
// #define RELAY_DEBUGGING
#define TESTING_NODE_ID 0

#define CURRENT_NODE_VERSION 2
#define BROADCAST_ADDR ESPNOW_BROADCAST_ADDRESS 

#define UPLINK_TIMEOUT 20000      // Time at which uplink is presumed lost
#define REBROADCAST_TIME 30000    // Time at which followers are presumed re-uplinked
#define WIFI_CHECK_RATE 2000     // Time at which we should check wifi status again

typedef uint16_t MeshId;

typedef struct {
    MeshId id = 0;
    MeshId uplinkId = 0;
    uint8_t version = CURRENT_NODE_VERSION;
} MeshNodeHeader;

typedef enum{
    RECIPIENTS_ALL=0,  // Send to all neighbors; non-followers will ignore
    RECIPIENTS_ROOT=1, // Send to root for rebroadcasting downward, all will see
    RECIPIENTS_INFO=2, // Send to all neighbors "FYI"; none will ignore
} MessageRecipients;

#define MESSAGE_DATA_SIZE 64

typedef struct {
    MeshNodeHeader header;
    MessageRecipients recipients;
    uint32_t timebase;
    CommandId command;
    byte data[MESSAGE_DATA_SIZE] = {0};
} NodeMessage;

typedef struct {
    uint8_t status;
    char message[40];
} NodeInfo;

void onDataReceived (uint8_t* address, uint8_t* data, uint8_t len, signed int rssi, bool broadcast);

const char *command_name(CommandId command) {
    switch (command) {
        case COMMAND_STATE:
            return "UPDATE";
        case COMMAND_OPTIONS:
            return "OPTIONS";
        case COMMAND_ACTION:
            return "ACTION";
        case COMMAND_INFO:
            return "INFO";
        case COMMAND_BEATS:
            return "BEATS";
        default:
            return "?COMMAND?";
    }
}

class MessageReceiver {
  public:
    virtual bool onCommand(CommandId command, void *data) {
      // Abstract: subclasses must define
      return false;
    }  
};

typedef enum{
    NODE_STATUS_QUIET=0,
    NODE_STATUS_STARTING=1,
    NODE_STATUS_STARTED=2,
} NodeStatus;


class LightNode {
  public:
    static LightNode* instance;

    MessageReceiver *receiver;
    MeshNodeHeader header;
    NodeStatus status = NODE_STATUS_QUIET;

    char node_name[20];

    Timer statusTimer;  // Use this timer to initialize and check wifi status
    Timer uplinkTimer; // When this timer ends, assume uplink is lost.
    Timer rebroadcastTimer; // Until this timer ends, re-broadcast messages from uplink

    LightNode(MessageReceiver *receiver) {
        LightNode::instance = this;

        this->receiver = receiver;
    }

    void onWifiConnect() {
        if (this->status == NODE_STATUS_QUIET)
            return;
            
        Serial.println("WiFi connected: stop broadcasting");
        quickEspNow.stop();
        this->status = NODE_STATUS_QUIET;
        this->rebroadcastTimer.stop();
        this->statusTimer.start(WIFI_CHECK_RATE);
    }

    void onWifiDisconnect() {
        if (this->status != NODE_STATUS_QUIET)
            return;

        Serial.println("WiFi disconnected: start broadcasting");
        WiFi.mode (WIFI_MODE_STA);
        WiFi.disconnect(false, true);
        quickEspNow.begin(1, WIFI_IF_STA);
        this->start();
    }

    void onMeshChange() {
        sprintf(this->node_name,
            "Tube %03X:%03X",
            this->header.id,
            this->header.uplinkId
        );
        this->configure_ap();
    }

    void configure_ap() {
#ifdef DEFAULT_WIFI
        strcpy(clientSSID, DEFAULT_WIFI);
        strcpy(clientPass, DEFAULT_WIFI_PASSWORD);
#else
        // Don't connect to any networks.
        strcpy(clientSSID, "");
        strcpy(clientPass, "");
#endif

        // By default, we don't want these visible.
        apBehavior = AP_BEHAVIOR_BUTTON_ONLY; // Must press button for 6 seconds to get AP
    }

    void start() {
        // Initialization timer: wait for a bit before trying to broadcast.
        // If this node's ID is high, it's more likely to be the leader, so wait less.
        this->status = NODE_STATUS_STARTING;
        this->statusTimer.start(3000 - this->header.id / 2);
        this->rebroadcastTimer.stop();
    }

    void onPeerPing(MeshNodeHeader* node) {
        // When receiving a message, if the IDs match, it's a conflict
        // Reset to create a new ID.
        if (node->id == this->header.id) {
            Serial.println("Detected an ID conflict.");
            this->reset();
        }

        // If the message arrives from a higher ID, switch into follower mode
        if (node->id > this->header.uplinkId && node->id > this->header.id) {            
#ifdef RELAY_DEBUGGING
          // When debugging relay, pretend not to see any nodes above 0x800
          if (node->id < 0x800)
#endif
            this->follow(node);
        }

        // If the message arrived from our uplink, track that we're still linked.
        if (node->id == this->header.uplinkId) {
            this->uplinkTimer.start(UPLINK_TIMEOUT);
        }

        // If a message indicates that another node is following this one, or
        // should be (it's not following anything, but this node's ID is higher)
        // enter or continue re-broadcasting mode.
        if (node->uplinkId == this->header.id
            || (node->uplinkId == 0 && node->id < this->header.id)) {
            Serial.printf("        %03X/%03X is following me\n", node->id, node->uplinkId);
            this->rebroadcastTimer.start(REBROADCAST_TIME);
        }
    }

    void print_message(NodeMessage* message, signed int rssi) {
        Serial.printf("%03X/%03X %s",
            message->header.id,
            message->header.uplinkId,
            command_name(message->command)
        );
        if (message->recipients == RECIPIENTS_ROOT)
            Serial.printf(":ROOT");
        if (rssi)
            Serial.printf(" %ddB ", rssi);
    }

    void onPeerData(uint8_t* address, uint8_t* data, uint8_t len, signed int rssi, bool broadcast) {
        // Ignore this message if it isn't a valid message payload.
        if (len != sizeof(NodeMessage))
            return;

        NodeMessage* message = (NodeMessage*)data;
        // Ignore this message if it's the wrong version.
        if (message->header.version != this->header.version) {
#ifdef NODE_DEBUGGING
            Serial.print("  -- !version ");
            print_message(message, rssi);
            Serial.println();
#endif
            return;
        }

        // Track that another node exists, updating this node's understanding of the mesh.
        this->onPeerPing(&message->header);

        bool ignore = false;
        switch (message->recipients) {
            case RECIPIENTS_ALL:
                // Ignore this message if not from the uplink
                ignore = (message->header.id != this->header.uplinkId);
                break;

            case RECIPIENTS_ROOT:
                // Ignore this message if not from one of this node's downlinks
                ignore = (message->header.uplinkId != this->header.id);
                break;

            case RECIPIENTS_INFO:
                ignore = false;
                break;

            default:
                // ignore this!
                ignore = true;
                break;
        }

        if (ignore) {
#ifdef NODE_DEBUGGING
            Serial.print("  -- ignored ");
            print_message(message, rssi);
            Serial.println();
#endif
            return;
        }

        // Execute the received command
        if (message->recipients != RECIPIENTS_ROOT || !this->is_following()) {
            Serial.print("  >> ");
            print_message(message, rssi);
            Serial.print(" ");

            // Adjust the timebase to match uplink
            // But only if it's drifting, else animations get jittery
            uint32_t new_timebase = message->timebase - millis() + 3; // Factor for network delay
            int32_t diff = new_timebase - strip.timebase;
            if (diff < -10 || diff > 10)
                strip.timebase = new_timebase;

            // Execute the command
            auto valid = this->receiver->onCommand(
                message->command,
                &message->data
            );
            Serial.println();

            if (!valid)
                return;
        }

        // Re-broadcast the message if appropriate
        if (!this->rebroadcastTimer.ended() && message->recipients != RECIPIENTS_INFO) {
            message->header = this->header;
            if (!this->is_following())
                message->recipients = RECIPIENTS_ALL;
            this->broadcast(message, true);
        }
    }

    void broadcast(NodeMessage *message, bool is_rebroadcast=false) {
        // Don't broadcast anything if this node isn't active.
        if (this->status != NODE_STATUS_STARTED)
            return;
        message->timebase = strip.timebase + millis();
        
#ifdef NODE_DEBUGGING
        Serial.print("  <<< ");
        print_message(message, 0);
        Serial.println();
#endif

        auto err = quickEspNow.send(
            ESPNOW_BROADCAST_ADDRESS,
            (uint8_t*)message, sizeof(*message)
        );
        if (err)
            Serial.printf("  *** Broadcast error %d\n", err);
    }

    void sendCommand(CommandId command, void *data, uint8_t len) {
        if (len > MESSAGE_DATA_SIZE) {
            Serial.printf("Message is too big: %d vs %d\n",
                len, MESSAGE_DATA_SIZE);
            return;
        }

        NodeMessage message;
        message.header = header;
        if (command == COMMAND_INFO) {
            message.recipients = RECIPIENTS_INFO;
        } else if (command == COMMAND_STATE) {
            message.recipients = RECIPIENTS_ALL;
        } else if (this->is_following()) {
            // Follower nodes must request that the root re-sends this message
            message.recipients = RECIPIENTS_ROOT;
        } else {
            message.recipients = RECIPIENTS_ALL;
        }
        message.command = command;
        memcpy(&message.data, data, len);
        this->broadcast(&message);
    }

    void setup() {
#ifdef NODE_DEBUGGING
        this->reset(TESTING_NODE_ID);
#else
        this->reset();
#endif
        this->statusTimer.stop();
        quickEspNow.onDataRcvd(onDataReceived);

        Serial.println("Mesh: ok");
    }

    void update() {
        // Check the last time we heard from the uplink node
        if (is_following() && this->uplinkTimer.ended()) {
            this->follow(NULL);
        }

        if (this->statusTimer.every(WIFI_CHECK_RATE)) {
            // The broadcast timer doubles as a timer for startup delay
            // Once the initial timer has ended, mark this node as started
            if (this->status == NODE_STATUS_STARTING)
                this->status = NODE_STATUS_STARTED;

            // Check WiFi status and update node status if wifi changed
            if (WiFi.isConnected())
                this->onWifiConnect();
            else
                this->onWifiDisconnect();
        }
    }

    void reset(MeshId id = 0) {
        if (id == 0)
            id = random(256, 4000);  // Leave room at bottom and top of 12 bits
        this->header.id = id;
        this->follow(NULL);
    }

    void follow(MeshNodeHeader* node) {
        if (node == NULL) {
            if (this->header.uplinkId != 0) {
                Serial.println("Uplink lost");
            }

            // Unfollow: following zero means you have no uplink
            this->header.uplinkId = 0;
            this->onMeshChange();
            return;
        }

        // Already following? ignore
        if (this->header.uplinkId == node->id)
            return;

        // Follow
        Serial.printf("Following %03X:%03X\n",
            node->id,
            node->uplinkId
        );
        this->header.uplinkId = node->id;
        this->onMeshChange();
    }

    bool is_following() {
        return this->header.uplinkId != 0;
    }
};

LightNode* LightNode::instance = nullptr;

void onDataReceived (uint8_t* address, uint8_t* data, uint8_t len, signed int rssi, bool broadcast) {
    LightNode::instance->onPeerData(address, data, len, rssi, broadcast);
}

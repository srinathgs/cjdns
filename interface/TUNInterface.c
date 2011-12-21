#include <string.h>
#include <event2/event.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>

#include "interface/Interface.h"
#include "interface/TUNInterface.h"
#include "libbenc/benc.h"

#define MAX_PACKET_SIZE 1500
#define PADDING_SPACE (2048 - MAX_PACKET_SIZE)

struct Tunnel
{
    struct event* incomingEvent;
    int fileDescriptor;
    struct Interface interface;
};

static int openTunnel(const char* interfaceName)
{
    fprintf(stderr, "Initializing tun device: ");
    if (interfaceName) {
        fprintf(stderr, "%s", interfaceName);
    }
    fprintf(stderr, "\n");

    int tunFileDescriptor;

    struct ifreq ifRequest;
    memset(&ifRequest, 0, sizeof(struct ifreq));
    ifRequest.ifr_flags = IFF_TUN | IFF_NO_PI;
    if (interfaceName) {
        strncpy(ifRequest.ifr_name, interfaceName, IFNAMSIZ);
    }
    tunFileDescriptor = open("/dev/net/tun", O_RDWR);

    if (tunFileDescriptor < 0) {
        return tunFileDescriptor;
    }

    int out = ioctl(tunFileDescriptor, TUNSETIFF, &ifRequest);
    if (out < 0) {
        close(tunFileDescriptor);
        return -1;
    }

    return tunFileDescriptor;
}

static void closeInterface(void* vcontext)
{
    struct Tunnel* tun = (struct Tunnel*) vcontext;
    close(tun->fileDescriptor);
    event_del(tun->incomingEvent);
    event_free(tun->incomingEvent);
}

static void handleEvent(evutil_socket_t socket, short eventType, void* vcontext)
{
    eventType = eventType;

    // 292 bytes of extra padding to build headers back from for better efficiency.
    uint8_t messageBuffer[MAX_PACKET_SIZE + PADDING_SPACE];

    struct Message message = {
        .bytes = messageBuffer + PADDING_SPACE,
        .padding = PADDING_SPACE,
        .length = MAX_PACKET_SIZE
    };

    ssize_t length = read(socket, messageBuffer + PADDING_SPACE, MAX_PACKET_SIZE);

    if (length < 0) {
        printf("Error reading from TUN device %d\n", (int) length);
        return;
    }
    message.length = length;

    struct Interface* iface = &((struct Tunnel*) vcontext)->interface;
    if (iface->receiveMessage) {
        iface->receiveMessage(&message, iface);
    }
}

static uint8_t sendMessage(struct Message* message, struct Interface* iface)
{
    struct Tunnel* tun = (struct Tunnel*) iface->senderContext;
    write(tun->fileDescriptor, message->bytes, message->length);
    // TODO: report errors
    return 0;
}

struct Interface* TunInterface_new(String* interfaceName,
                                   struct event_base* base,
                                   struct MemAllocator* allocator)
{
    errno = 0;
    int tunFileDesc = openTunnel(interfaceName ? interfaceName->bytes : NULL);
    if (tunFileDesc < 0) {
        if (errno == EPERM) {
            fprintf(stderr, "You don't have permission to open tunnel. "
                            "This probably needs to be run as root.\n");
        } else {
            fprintf(stderr, "Failed to open tunnel, error: %d\n", errno);
        }
        return NULL;
    }

    evutil_make_socket_nonblocking(tunFileDesc);

    struct Tunnel* tun = allocator->malloc(sizeof(struct Tunnel), allocator);
    tun->incomingEvent = event_new(base, tunFileDesc, EV_READ | EV_PERSIST, handleEvent, tun);
    tun->fileDescriptor = tunFileDesc;

    if (tun->incomingEvent == NULL) {
        abort();
    }

    struct Interface iface = {
        .senderContext = tun,
        .sendMessage = sendMessage,
        .allocator = allocator,
        .requiredPadding = 0,
        .maxMessageLength = MAX_PACKET_SIZE
    };

    memcpy(&tun->interface, &iface, sizeof(struct Interface));

    event_add(tun->incomingEvent, NULL);

    allocator->onFree(closeInterface, tun, allocator);

    return &tun->interface;
}
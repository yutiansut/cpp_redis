#ifndef CPP_REDIS_WINSOCK_INITIALIZER_H_
#define CPP_REDIS_WINSOCK_INITIALIZER_H_

#ifdef _WIN32
#include <Winsock2.h>
#include <stdexcept>

class winsock_initializer {
public:
  winsock_initializer() {
    //! Windows network DLL init
    WORD version = MAKEWORD(2, 2);
    WSADATA data;

    if (WSAStartup(version, &data) != 0) {
      throw std::runtime_error("WSAStartup() failure");
    }
  }
  ~winsock_initializer() { WSACleanup(); }
};

#define WINSOCK_INITIALIZER() winsock_initializer winsock_init;
#else
class winsock_initializer {};
#define WINSOCK_INITIALIZER()
#endif /* _WIN32 */

#endif // !CPP_REDIS_WINSOCK_INITIALIZER_H_
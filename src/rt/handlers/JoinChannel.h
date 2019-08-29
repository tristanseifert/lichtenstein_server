//
// Created by Tristan Seifert on 2019-08-22.
//

#ifndef LICHTENSTEIN_SERVER_RT_HANDLERS_JOINCHANNEL_H
#define LICHTENSTEIN_SERVER_RT_HANDLERS_JOINCHANNEL_H

#include "../IRequestHandler.h"

#include <memory>

namespace rt {
  class ClientHandler;
}

namespace rt::handlers {
  class JoinChannel : public IRequestHandler {
    public:
      explicit JoinChannel(ClientHandler *client) : IRequestHandler(client) {}

    public:
      void handle(const lichtenstein::protocol::Message &received) override;

    private:
      static std::unique_ptr<IRequestHandler>
      construct(ClientHandler *client);

    private:
      static bool registered;
  };
}


#endif //LICHTENSTEIN_SERVER_RT_HANDLERS_JOINCHANNEL_H
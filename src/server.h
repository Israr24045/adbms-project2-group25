#pragma once
#include <string>

class Server {
public:
    Server(const std::string& data_dir, int port);
    void run();

private:
    std::string data_dir_;
    int port_;
};
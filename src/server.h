#pragma once

#include <string>
#include "storage.h"

using namespace std;

class Server {
public:
    Server(const string& data_dir, int port);
    void run();   

private:
    
    void handle_client(int client_fd);

    string     data_dir_;
    int             port_;
    MetricRegistry  registry_;
};

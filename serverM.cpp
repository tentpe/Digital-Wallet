#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <string>
#include <iostream>
#include <vector>
#include <poll.h>
#include <fstream>
#include <pthread.h>
#include <map>
#include <sstream>
#include <algorithm>

#define localhost "127.0.0.1"

#define UDP_PORT_SERVER_A "21256"
#define UDP_PORT_SERVER_B "22256"
#define UDP_PORT_SERVER_C "23256"
#define SLAVE_SERVER_SIZE 3

#define UDP_PORT_SERVER_M "24256"
#define TCP_PORT_CLIENT_A "25256"
#define TCP_PORT_CLIENT_B "26256"
#define CLIENT_SIZE 2

#define BUF_SIZE 2048
#define FLAG 0

#define CHECK_WALLET 0
#define TX_LIST -1
#define LOGGED -2
#define STATS -3

#define INITIAL_BALANCE 1000
#define TX_LIST_FILE_PATH "./alichain.txt"
#define LIS_MAX_SIZE 3000

using namespace std;

struct Transaction {
    int serial_number;
    string sender;
    string receiver;
    int amount;
};

// getting result from servers
struct QueryResult {
    int size;
    Transaction transaction_list[LIS_MAX_SIZE];
};

// record all transactions
vector<struct Transaction> transaction_list;
// record stats
vector<struct Transaction> stats_list;

// receive input
char recv_buffer[BUF_SIZE];
Transaction query;
QueryResult query_result;

Transaction operations[CLIENT_SIZE];
QueryResult operation_results[CLIENT_SIZE];

// tcp for clients
int tcp_listener[CLIENT_SIZE];
const char* tcp_port_list[CLIENT_SIZE];

// udp for servers
int udp_listener;
int udp_talkers[SLAVE_SERVER_SIZE];
struct addrinfo* slave_server_info_list[SLAVE_SERVER_SIZE];
const char* udp_port_list[SLAVE_SERVER_SIZE];

int max_serial_number = 0;

// refer to https://beej.us/guide/bgnet/examples/listener.c
void start_udp_listener() {
    struct addrinfo hints, *servinfo, *p;
    int rv;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    if ((rv = getaddrinfo(NULL, UDP_PORT_SERVER_M, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        exit(1);
    }

    // create socket
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((udp_listener = socket(p->ai_family, p->ai_socktype,
                                   p->ai_protocol)) == -1) {
            continue;
        }

        // binding
        if (::bind(udp_listener, p->ai_addr, p->ai_addrlen) == -1) {
            close(udp_listener);
            continue;
        }

        break;
    }

    if (p == NULL) {
        exit(2);
    }

    freeaddrinfo(servinfo);
}

// refer to https://beej.us/guide/bgnet/examples/server.c
// start up tcp
void start_tcp_listener(int client_index) {
    int yes = 1;
    int rv;

    struct addrinfo hints, *servinfo, *p;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if ((rv = getaddrinfo(NULL, tcp_port_list[client_index], &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        exit(1);
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        tcp_listener[client_index] = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (tcp_listener[client_index] < 0) {
            continue;
        }

        if (setsockopt(tcp_listener[client_index], SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            perror("setsockopt");
            exit(1);
        }

        if (::bind(tcp_listener[client_index], p->ai_addr, p->ai_addrlen) < 0) {
            close(tcp_listener[client_index]);
            perror("server: bind");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo);

    if ((p == NULL) || (listen(tcp_listener[client_index], 10) == -1)) {
        exit(-1);
    }
}

// refer to https://beej.us/guide/bgnet/examples/talker.c
// udp with servers
void start_udp_talker(int slave_index) {
    struct addrinfo hints, *servinfo, *p;
    int rv;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; // set to AF_INET to use IPv4
    hints.ai_socktype = SOCK_DGRAM;

    if ((rv = getaddrinfo(localhost, udp_port_list[slave_index], &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        exit(1);
    }

    // loop through all the results and make a socket
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((udp_talkers[slave_index] = socket(p->ai_family, p->ai_socktype,
                             p->ai_protocol)) == -1) {
            perror("talker: socket");
            continue;
        }

        break;
    }

    if (p == NULL) {
        fprintf(stderr, "talker: failed to create socket\n");
        exit(2);
    }

    slave_server_info_list[slave_index] = p;
}

void boot_up_serverM() {
    start_udp_listener();

    udp_port_list[0] = UDP_PORT_SERVER_A;
    udp_port_list[1] = UDP_PORT_SERVER_B;
    udp_port_list[2] = UDP_PORT_SERVER_C;
    start_udp_talker(0);
    start_udp_talker(1);
    start_udp_talker(2);

    tcp_port_list[0] = TCP_PORT_CLIENT_A;
    tcp_port_list[1] = TCP_PORT_CLIENT_B;

    start_tcp_listener(0);
    start_tcp_listener(1);

    cout << "The main server is up and running." << endl;
}

// pass transactions to servers
void talk_to_slave_server(int i) {
    sockaddr *slave_server_addr = slave_server_info_list[i]->ai_addr;
    socklen_t slave_server_addrlen = slave_server_info_list[i]->ai_addrlen;

    stringstream ss;
    ss << query.serial_number << " " << query.sender << " " << query.receiver << " " << query.amount;

    int sendto_result = sendto(udp_talkers[i], ss.str().c_str(),ss.str().length(), FLAG, slave_server_addr,
                               slave_server_addrlen);
    if (sendto_result == -1) {
        exit(1);
    }

    cout << "The main server sent a request to server " << (char) ('A' + i) << "." << endl;
}

// get results from servers
void listen_from_slave_server(int i) {
    memset(recv_buffer, 0, BUF_SIZE);
    struct sockaddr_storage their_addr;
    socklen_t addr_len = sizeof their_addr;
    int recv_result = recvfrom(udp_listener, &recv_buffer, BUF_SIZE, FLAG, (struct sockaddr *) &their_addr,
                               &addr_len);
    if (recv_result == -1) {
        perror("recvfrom error");
        exit(1);
    }

    //memset(&query_result, 0, sizeof(query_result));
    //memcpy(&query_result, recv_buffer, sizeof(query_result));

    istringstream iss(recv_buffer);
    // maximum serial number
    int max_serial;
    iss >> max_serial;
    iss >> query_result.size;
    // loop throught to get all the transactions related
    for (int f = 0; f < query_result.size; f++) {
        iss >> query_result.transaction_list[f].serial_number;
        iss >> query_result.transaction_list[f].sender;
        iss >> query_result.transaction_list[f].receiver;
        iss >> query_result.transaction_list[f].amount;
    }

    if (max_serial_number < max_serial) {
        max_serial_number = max_serial;
    }

    cout << "The main server received the feedback from server " << (char) ('A' + i) << " using UDP over port "
         << udp_port_list[i] << "." << endl;
}

// used to sort by serial number
bool compareTransaction(Transaction t1, Transaction t2) {
    return (t1.serial_number < t2.serial_number);
}

// function to talk and listen to severs
// check the current balance

int check_wallet(string username) {
    int balance = INITIAL_BALANCE;

    query.serial_number = CHECK_WALLET;
    query.sender = username;

    for (int i = 0; i < SLAVE_SERVER_SIZE; i++) {
        talk_to_slave_server(i);
        listen_from_slave_server(i);

        // calculate balance
        for (int k = 0; k < query_result.size; k++) {
            Transaction t = query_result.transaction_list[k];
            if (t.sender == username) {
                balance -= t.amount;
            }
            if (t.receiver == username) {
                balance += t.amount;
            }
        }
    }
    
    return balance;
}

// function to tranfer coins

bool tx_coins(string sender, string receiver, int amount) {
    int sender_balance = check_wallet(sender);
    if (sender_balance < amount) {
        query_result.size = sender_balance;
        return false;
    }
    // calculate the serial number to add
    int serial_number = max_serial_number + 1;
    query.serial_number = serial_number;
    query.sender = sender;
    query.receiver = receiver;
    query.amount = amount;

    int server_index = serial_number % 3;
    talk_to_slave_server(server_index);
    listen_from_slave_server(server_index);
    // result size equals to loggged to decide if transfer coins
    if (query_result.size == LOGGED) {
        max_serial_number += 1;
        sender_balance -= amount;
        query_result.size = sender_balance;
        return true;
    }

    return false;
}

// get all the transactions and record them in transaction_list
void get_all_transactions() {
    transaction_list.clear();

    query.serial_number = TX_LIST;

    for (int i = 0; i < SLAVE_SERVER_SIZE; i++) {
        // get all transaction
        talk_to_slave_server(i);
        listen_from_slave_server(i);
        // store them
        for (int k = 0; k < query_result.size; k++) {
            Transaction t = query_result.transaction_list[k];
            if (t.serial_number > max_serial_number) {
                max_serial_number = t.serial_number;
            }
            transaction_list.push_back(t);
        }
    }
}

// operation 3, out put sorted transactions by serial

void output_tx_list() {
    ofstream output_file;
    output_file.open(TX_LIST_FILE_PATH);

    for (int i = 0; i < transaction_list.size(); i++) {
        Transaction t = transaction_list.at(i);
        if (t.serial_number == 0) continue; // skip empty input
        output_file << t.serial_number;
        output_file << " ";
        output_file << t.sender;
        output_file << " ";
        output_file << t.receiver;
        output_file << " ";
        output_file << t.amount;
        output_file << "\n"; //write transaction details and \n
    }
    output_file.close();
}

void tx_list() {
    get_all_transactions();
    // sort by serial
    sort(transaction_list.begin(), transaction_list.end(), compareTransaction);
    output_tx_list();
}

// sorting transactions based on number of transactions, serial number taken for number of transactions
bool compare_stats(Transaction s1, Transaction s2) {
    return (s1.serial_number > s2.serial_number);
}

void stats(string username) {
    get_all_transactions();
    map<string, Transaction> user_stats_map;
    for (int i = 0; i < transaction_list.size(); i++) {
        Transaction t = transaction_list.at(i);
        if (t.serial_number == 0) continue;
        // if sender is user, store receiver's info
        if (t.sender == username) {
            map<string, Transaction>::iterator it = user_stats_map.find(t.receiver);
            if (it != user_stats_map.end()) {
                Transaction temp;
                user_stats_map.insert(pair<string, Transaction>(t.receiver, temp));
            }
            user_stats_map[t.receiver].amount -= t.amount;
            user_stats_map[t.receiver].serial_number++;
        }
        // if receiver is user, store sender's info
        else if (t.receiver == username) {
            map<string, Transaction>::iterator it = user_stats_map.find(t.sender);
            if (it != user_stats_map.end()) {
                Transaction temp;
                user_stats_map.insert(pair<string, Transaction>(t.sender, temp));
            }
            user_stats_map[t.sender].amount += t.amount;
            user_stats_map[t.sender].serial_number++;
        }
    }
    stats_list.clear();
    // store stats info
    for (map<string, Transaction>::iterator it = user_stats_map.begin(); it != user_stats_map.end(); it++) {
        Transaction stats;
        stats.sender = it->first;
        stats.serial_number = it->second.serial_number;
        stats.amount = it->second.amount;
        stats_list.push_back(stats);
    }
    // sort stats info by number of transactions
    sort(stats_list.begin(), stats_list.end(), compare_stats);
}

void* handle_client_operations(void* param) {
    int client_index = *((int*)param);

    struct sockaddr_storage their_addr;
    socklen_t sin_size;
    sin_size = sizeof their_addr;

    char buf[BUF_SIZE];
    while (true) {
        // accept
        int new_fd = accept(tcp_listener[client_index], (struct sockaddr *) &their_addr, &sin_size);
        if (new_fd == -1) {
            perror("accept");
            continue;
        }

        //memset(&operation_results[client_index], 0, sizeof(operation_results[client_index]));
        memset(&query, 0, sizeof(query));
        //memset(&query_result, 0, sizeof(query_result));

        memset(&buf, 0, BUF_SIZE);
        read(new_fd, buf, BUF_SIZE);
        //memset(&operations[client_index], 0, sizeof(operations[client_index]));
        //memcpy(&operations[client_index], buf, sizeof(operations[client_index]));

        istringstream iss(buf);
        iss >> operations[client_index].serial_number;

        // define integer to serial number to decide operations
        if (operations[client_index].serial_number == CHECK_WALLET) {
            iss >> operations[client_index].sender;
            cout << "The main server received input=\"" << operations[client_index].sender
                 << "\" from the client using TCP over port " << tcp_port_list[client_index] << "." << endl;
            int balance = check_wallet(operations[client_index].sender);
            operation_results[client_index].size = balance;

            stringstream ss;
            ss << operation_results[client_index].size;

            if (send(new_fd, ss.str().c_str(),ss.str().length(), 0) == -1)
                perror("send");
            cout << "The main server sent the current balance to client " << (char) ('A' + client_index) << "." << endl;
        }

        // transfer coins
        else if (operations[client_index].serial_number > CHECK_WALLET) {
            get_all_transactions();
            iss >> operations[client_index].sender;
            iss >> operations[client_index].receiver;
            iss >> operations[client_index].amount;
            bool sender_in = false;
            bool receiver_in = false;
            for (int i = 0; i < transaction_list.size(); i++) {
                if (transaction_list.at(i).sender == operations[client_index].sender || transaction_list.at(i).receiver == operations[client_index].sender) {
                    sender_in = true;
                }
                if (transaction_list.at(i).sender == operations[client_index].receiver || transaction_list.at(i).receiver == operations[client_index].receiver) {
                    receiver_in = true;
                }
            }
            // neither sender receiver is in network
            if (!sender_in && !receiver_in) {
                stringstream ss;
                operation_results[client_index].size = -999;
                operation_results[client_index].transaction_list[0].amount = INITIAL_BALANCE;
                ss << operation_results[client_index].size << " ";
                ss << operation_results[client_index].transaction_list[0].amount;
                if (send(new_fd, ss.str().c_str(),ss.str().length(), 0) == -1)
                perror("send");
            }
            // sender is not in net
            else if (!sender_in && receiver_in) {
                stringstream ss;
                operation_results[client_index].size = -888;
                operation_results[client_index].transaction_list[0].amount = INITIAL_BALANCE;
                ss << operation_results[client_index].size << " ";
                ss << operation_results[client_index].transaction_list[0].amount;
                if (send(new_fd, ss.str().c_str(),ss.str().length(), 0) == -1)
                perror("send");
            }
            // receiver is not in net
            else if (sender_in && !receiver_in) {
                stringstream ss;
                operation_results[client_index].size = -777;
                int balance = check_wallet(operations[client_index].sender);
                operation_results[client_index].transaction_list[0].amount = balance;
                ss << operation_results[client_index].size << " ";
                ss << operation_results[client_index].transaction_list[0].amount;
                if (send(new_fd, ss.str().c_str(),ss.str().length(), 0) == -1)
                perror("send");
            }
            // both in net
            else {
                cout << "The main server received from \"" << operations[client_index].sender << "\" to transfer "
                    << operations[client_index].amount << " coins to \"" << operations[client_index].receiver
                    << "\" using TCP over port " << tcp_port_list[client_index] << "." << endl;
                bool is_success = tx_coins(operations[client_index].sender, operations[client_index].receiver,
                                        operations[client_index].amount);
                if (is_success) {
                    operation_results[client_index].size = 1;
                    operation_results[client_index].transaction_list[0].amount = query_result.size;
                } else {
                    operation_results[client_index].size = 0;
                    operation_results[client_index].transaction_list[0].amount = query_result.size;
                }

                stringstream ss;
                ss << operation_results[client_index].size << " ";
                ss << operation_results[client_index].transaction_list[0].amount;

                if (send(new_fd, ss.str().c_str(),ss.str().length(), 0) == -1)
                    perror("send");
                cout << "The main server sent the result of the transaction to client " << (char) ('A' + client_index) << "." << endl;
            }
        }

        // sort list
        else if (operations[client_index].serial_number == TX_LIST) {
            tx_list();
            operation_results[client_index].size = 1;

            stringstream ss;
            ss << operation_results[client_index].size;

            if (send(new_fd, ss.str().c_str(),ss.str().length(), 0) == -1)
                perror("send");
        }

        // send statas
        else if (operations[client_index].serial_number == STATS) {
            iss >> operations[client_index].sender;
            stats(operations[client_index].sender);
            operation_results[client_index].size = stats_list.size();
            for (int i = 0; i < stats_list.size(); i++) {
                operation_results[client_index].transaction_list[i] = stats_list.at(i);
            }

            stringstream ss;
            ss << operation_results[client_index].size << " ";
            for (int j = 0; j < operation_results[client_index].size; j++) {
                ss << operation_results[client_index].transaction_list[j].serial_number << " " 
                   << operation_results[client_index].transaction_list[j].sender << " " 
                   << operation_results[client_index].transaction_list[j].amount << " ";
            }

            if (send(new_fd, ss.str().c_str(),ss.str().length(), 0) == -1)
                perror("send");
        }
    }
}

int main() {
    boot_up_serverM();
    while(true) {
        // pthread for multi thread cocurrently
        pthread_t clientA_handler;
        pthread_t clientB_handler;
        int index_A = 0;
        pthread_create(&clientA_handler, NULL, handle_client_operations, (void *)&(index_A));
        int index_B = 1;
        pthread_create(&clientB_handler, NULL, handle_client_operations, (void *)&(index_B));

        pthread_join(clientA_handler, NULL);
        pthread_join(clientB_handler, NULL);
    }
    return 0;
}


void* clients_get();
void* clients_setup();
void mock_client_setup_disk_backend(void* bt, unsigned int piece_len);
void client_add_peer(
    client_t* me,
    char* peer_id,
    unsigned int peer_id_len,
    char* ip,
    unsigned int ip_len,
    unsigned int port);
client_t* mock_client_setup(int piecelen);

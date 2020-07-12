#include "broker.h"

struct buddy {
	uint32_t size;
	uint32_t longest[1];
};

static inline int left_child(int index) {
	// index * 2^1 + 1
	return ((index << 1) + 1);
}

static inline int right_child(int index) {
	// index * 2^1 + 2
	return ((index << 1) + 2);
}

static inline int parent(int index) {
	// (index+1)/2^1 - 1
	return (((index + 1) >> 1) - 1);
}

static inline bool is_power_of_2(int index) {
	return !(index & (index - 1));
}

static inline unsigned next_power_of_2(int size) {
	size -= 1;
	size |= (size >> 1);
	size |= (size >> 2);
	size |= (size >> 4);
	size |= (size >> 8);
	size |= (size >> 16);
	return size + 1;
}

void signal_handler(int signum) {
	if (signum == SIGUSR1) {
		dump();
	}
}

int main(int argc, char *argv[]) {
	initialize_queue();
	if (broker_load() < 0)
		return EXIT_FAILURE;
	memory = malloc(broker_config->tamano_memoria);
	memset(memory, '\0', broker_config->tamano_memoria);

	if (broker_config->estrategia_memoria == 0) {
		// Init buddy system structure
		buddy = buddy_new(broker_config->tamano_memoria);
	}

	pointer = 0;
	broker_server_init();
	broker_exit();

	return EXIT_SUCCESS;
}

// Buddy-related code

/** Allocate a new buddy structure
 * @param total_mem total memory to be managed
 * @return pointer to the allocated buddy structure */
struct buddy *buddy_new(int total_mem) {
	struct buddy *self = NULL;
	uint32_t node_size;

	int i;

	if (!is_power_of_2(total_mem)) {
		total_mem = next_power_of_2(total_mem);
	}

	// Allocate an array to represent a complete binary tree
	self = (struct buddy *) malloc(
			sizeof(struct buddy) + 2 * total_mem * sizeof(uint32_t));
	self->size = total_mem;
	node_size = total_mem * 2;

	/* Initialize longest array for buddy structure */
	int iter_end = total_mem * 2 - 1;
	for (i = 0; i < iter_end; i++) {
		if (is_power_of_2(i + 1)) {
			node_size >>= 1;
		}
		self->longest[i] = node_size;
	}

	return self;
}

void buddy_destroy(struct buddy *self) {
	free(self);
}

// Choose smallest child greater than size
unsigned choose_better_child(struct buddy *self, int index, uint32_t size) {
	struct compound {
		uint32_t size;
		int index;
	} children[2];

	children[0].index = left_child(index);
	children[0].size = self->longest[children[0].index];
	children[1].index = right_child(index);
	children[1].size = self->longest[children[1].index];

	int min_idx = (children[0].size <= children[1].size) ? 0 : 1;

	if (size > children[min_idx].size) {
		min_idx = 1 - min_idx;
	}

	return children[min_idx].index;
}

/** @param size to alloc
 * @param self source buddy system
 * @return the offset from the beginning of memory to be managed */
int buddy_alloc(struct buddy *self, uint32_t size) {
	if (self == NULL || self->size < size) {
		broker_logger_error(
				"Data couldn't be stored. Reason: Insufficient memory");
		return -1;
	}
	size = next_power_of_2(size);

	uint32_t index = 0;
	while (self->longest[index] < size) {
		broker_logger_error(
				"Data couldn't be stored. Reason: External fragmentation");
		broker_logger_info("Freeing memory...");
		purge_msg();
	}

	// Search recursively for the child
	uint32_t node_size = 0;
	for (node_size = self->size; node_size != size; node_size >>= 1) {
		index = choose_better_child(self, index, size);
	}

	// Update the longest value back
	self->longest[index] = 0;
	int offset = (index + 1) * node_size - self->size;

	while (index) {
		index = parent(index);
		self->longest[index] = max(self->longest[left_child(index)],
				self->longest[right_child(index)]);
	}

	return offset;
}

void buddy_free(struct buddy *self, int offset) {
	if (self == NULL || offset < 0 || offset > self->size) {
		broker_logger_error("Error: value out of bounds");
		return;
	}

	uint32_t node_size;
	uint32_t index;

	// Get the corresponding index from offset
	node_size = 1;
	index = offset + self->size - 1;

	for (; self->longest[index] != 0; index = parent(index)) {
		node_size <<= 1; // node_size *= 2

		if (index == 0) {
			break;
		}
	}

	self->longest[index] = node_size;

	while (index) {
		index = parent(index);
		node_size <<= 1;

		uint32_t left_longest = self->longest[left_child(index)];
		uint32_t right_longest = self->longest[right_child(index)];

		if (left_longest + right_longest == node_size) {
			self->longest[index] = node_size;
		} else {
			self->longest[index] = max(left_longest, right_longest);
		}
	}
}

// Broker init
int broker_load() {
	int response = broker_config_load();
	if (response < 0)
		return response;

	response = broker_logger_create(broker_config->log_file);
	if (response < 0) {
		broker_config_free();
		return response;
	}
	broker_print_config();

	// Create mutex for pointer
	if (pthread_mutex_init(&mpointer, NULL) != 0) {
		printf("\n mutex init failed\n");
		return 1;
	}
	if (pthread_mutex_init(&mid, NULL) != 0) {
		printf("\n mutex init failed\n");
		return 1;
	}

	return 0;
}

void broker_server_init() {

	base_time = time(NULL);
	broker_socket = socket_create_listener(broker_config->ip_broker,
			broker_config->puerto_broker);
	if (broker_socket < 0) {
		broker_logger_error("Error al crear server");
		return;
	}

	signal(SIGUSR1, signal_handler);
	broker_logger_info("Server creado correctamente!! Esperando conexiones...");

	struct sockaddr_in client_info;
	socklen_t addrlen = sizeof client_info;

	pthread_attr_t attrs;
	pthread_attr_init(&attrs);
	pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_JOINABLE);

	int accepted_fd;
	for (;;) {

		pthread_t tid;
		if ((accepted_fd = accept(broker_socket,
				(struct sockaddr *) &client_info, &addrlen)) != -1) {

			broker_logger_info(
					"Creando un hilo para atender una conexión en el socket %d",
					accepted_fd);
			pthread_create(&tid, NULL, (void*) handle_connection,
					(void*) &accepted_fd);
			pthread_detach(tid);
			usleep(500000);
		} else {
			broker_logger_error("Error al conectar con un cliente");
		}
	}
}

static void *handle_connection(void *arg) {
	int client_fd = *((int *) arg);

	t_protocol new_protocol;
	t_protocol catch_protocol;
	t_protocol caught_protocol;
	t_protocol get_protocol;
	t_protocol appeared_protocol;
	t_protocol localized_protocol;

	broker_logger_info("Conexion establecida con cliente: %d", client_fd);

	int received_bytes;
	int protocol;
	while (true) {
		received_bytes = recv(client_fd, &protocol, sizeof(int), 0);

		if (received_bytes <= 0) {
			broker_logger_error("Se perdio la conexion");
			return NULL;
		}
		switch (protocol) {

		// From GB
		case NEW_POKEMON: {
			broker_logger_info("NEW RECEIVED FROM GB");
			t_new_pokemon *new_receive = utils_receive_and_deserialize(
					client_fd, protocol);
			new_receive->id_correlacional = generar_id();
			broker_logger_info("ID Correlacional: %d",
					new_receive->id_correlacional);
			broker_logger_info("Cantidad: %d", new_receive->cantidad);
			broker_logger_info("Nombre Pokemon: %s",
					new_receive->nombre_pokemon);
			broker_logger_info("Largo Nombre: %d", new_receive->tamanio_nombre);
			broker_logger_info("Posicion X: %d", new_receive->pos_x);
			broker_logger_info("Posicion Y: %d", new_receive->pos_y);
			usleep(100000);
			t_message_to_void *message_void = convert_to_void(protocol,
					new_receive);
			int from = save_on_memory(message_void);
			broker_logger_info("STARTING POSITION FOR NEW_POKEMON: %d", from);
			save_node_list_memory(from, message_void->size_message, NEW_QUEUE,
					new_receive->id_correlacional);
			t_new_pokemon* new_snd = get_from_memory(protocol, from, memory);
			new_snd->id_correlacional = new_receive->id_correlacional;
			create_message_ack(new_snd->id_correlacional, new_queue, NEW_QUEUE);

			// To GC
			new_protocol = NEW_POKEMON;
			broker_logger_info("NEW SENT TO GC");

			_Bool node_matches_new(t_subscribe_message_node* node) {
				return node->id == new_snd->id_correlacional
						&& node->cola == NEW_QUEUE;
			}

			void send_msg_to_sub(t_subscribe_nodo* node) {
				_Bool subscriber_ack_matches_new(t_subscribe_ack_node* n) {
					return n->subscribe->f_desc == node->f_desc;
				}

				utils_serialize_and_send(node->f_desc, new_protocol, new_snd);

				t_protocol rcv_int;
				int r = recv(node->f_desc, &rcv_int, sizeof(t_protocol),
				MSG_WAITALL);
				broker_logger_warn("%d", r);
				broker_logger_warn("ACK Received");

				t_subscribe_message_node* node_ack = list_find(
						list_msg_subscribers, (void*) node_matches_new);
				t_subscribe_ack_node* node_subscriber = list_find(
						node_ack->list, (void*) subscriber_ack_matches_new);
				node_subscriber->ack = true;
			}

			for (int i = 0; i < list_size(new_queue); i++) {
				t_subscribe_nodo* node = list_get(new_queue, i);

				pthread_t ack_new_tid;
				pthread_create(&ack_new_tid, NULL, (void*) send_msg_to_sub,
						(void*) node);
				pthread_detach(ack_new_tid);
				usleep(100000);
			}
			usleep(50000);
			break;
		}

			// From GB or GC
		case APPEARED_POKEMON: {
			broker_logger_info("APPEARED RECEIVED");
			t_appeared_pokemon *appeared_rcv = utils_receive_and_deserialize(
					client_fd, protocol);
			broker_logger_info("ID correlacional: %d",
					appeared_rcv->id_correlacional);
			broker_logger_info("Cantidad: %d", appeared_rcv->cantidad);
			broker_logger_info("Nombre Pokemon: %s",
					appeared_rcv->nombre_pokemon);
			broker_logger_info("Largo nombre: %d",
					appeared_rcv->tamanio_nombre);
			broker_logger_info("Posicion X: %d", appeared_rcv->pos_x);
			broker_logger_info("Posicion Y: %d", appeared_rcv->pos_y);
			usleep(50000);
			t_message_to_void *message_void = convert_to_void(protocol,
					appeared_rcv);
			int from = save_on_memory(message_void);
			broker_logger_info("STARTING POSITION FOR APPEARED_POKEMON: %d",
					from);
			save_node_list_memory(from, message_void->size_message,
					APPEARED_QUEUE, appeared_rcv->id_correlacional);
			t_appeared_pokemon* appeared_snd = get_from_memory(protocol, from,
					memory);
			create_message_ack(id, appeared_queue, APPEARED_QUEUE);

			appeared_snd->id_correlacional = appeared_rcv->id_correlacional;

			// To Team
			appeared_protocol = APPEARED_POKEMON;
			broker_logger_info("APPEARED SENT TO TEAM");

			_Bool node_matches_appeared(t_subscribe_message_node* node) {
				return node->id == appeared_snd->id_correlacional
						&& node->cola == NEW_QUEUE;
			}

			void send_msg_to_sub(t_subscribe_nodo* node) {
				_Bool subscriber_ack_matches_appeared(t_subscribe_ack_node* n) {
					return n->subscribe->f_desc == node->f_desc;
				}

				utils_serialize_and_send(node->f_desc, appeared_protocol,
						appeared_snd);

				t_protocol rcv_int;
				int r = recv(node->f_desc, &rcv_int, sizeof(t_protocol),
				MSG_WAITALL);
				broker_logger_warn("%d", r);
				broker_logger_warn("ACK Received");

				t_subscribe_message_node* node_ack = list_find(
						list_msg_subscribers, (void*) node_matches_appeared);
				t_subscribe_ack_node* node_subscriber = list_find(
						node_ack->list,
						(void*) subscriber_ack_matches_appeared);
				node_subscriber->ack = true;
			}

			for (int i = 0; i < list_size(appeared_queue); i++) {
				t_subscribe_nodo* node = list_get(appeared_queue, i);

				pthread_t ack_appeared_tid;
				pthread_create(&ack_appeared_tid, NULL, (void*) send_msg_to_sub,
						(void*) node);
				pthread_detach(ack_appeared_tid);
				usleep(100000);
			}
			usleep(500000);
			break;
		}

			// From team
		case GET_POKEMON: {
			broker_logger_info("GET RECEIVED FROM TEAM");
			t_get_pokemon *get_rcv = utils_receive_and_deserialize(client_fd,
					protocol);
			broker_logger_info("Nombre Pokemon: %s", get_rcv->nombre_pokemon);
			broker_logger_info("Largo nombre: %d", get_rcv->tamanio_nombre);
			get_rcv->id_correlacional = generar_id();
			broker_logger_info("ID correlacional: %d",
					get_rcv->id_correlacional);
			usleep(50000);

			t_message_to_void *message_void = convert_to_void(protocol,
					get_rcv);
			int from = save_on_memory(message_void);
			broker_logger_info("STARTING POSITION FOR GET_POKEMON: %d", from);
			save_node_list_memory(from, message_void->size_message, GET_QUEUE,
					get_rcv->id_correlacional);
			t_get_pokemon* get_snd = get_from_memory(protocol, from, memory);
			get_snd->id_correlacional = get_rcv->id_correlacional;

			// TODO: Mandar el id generado al team
			send(client_fd, &get_rcv->id_correlacional, sizeof(uint32_t), 0);

			create_message_ack(get_rcv->id_correlacional, get_queue, GET_QUEUE);

			// To GC
			get_protocol = GET_POKEMON;
			broker_logger_info("GET SENT TO GAMECARD");

			_Bool node_matches_get(t_subscribe_message_node* node) {
				return node->id == get_snd->id_correlacional
						&& node->cola == GET_QUEUE;
			}

			void send_msg_to_sub(t_subscribe_nodo* node) {
				_Bool subscriber_ack_matches_get(t_subscribe_ack_node* n) {
					return n->subscribe->f_desc == node->f_desc;
				}

				utils_serialize_and_send(node->f_desc, get_protocol, get_snd);

				t_protocol rcv_int;
				int r = recv(node->f_desc, &rcv_int, sizeof(t_protocol),
				MSG_WAITALL);
				broker_logger_warn("%d", r);
				broker_logger_warn("ACK Received");

				t_subscribe_message_node* node_ack = list_find(
						list_msg_subscribers, (void*) node_matches_get);
				t_subscribe_ack_node* node_subscriber = list_find(
						node_ack->list, (void*) subscriber_ack_matches_get);
				node_subscriber->ack = true;
			}

			for (int i = 0; i < list_size(get_queue); i++) {
				t_subscribe_nodo* node = list_get(get_queue, i);

				pthread_t ack_get_tid;
				pthread_create(&ack_get_tid, NULL, (void*) send_msg_to_sub,
						(void*) node);
				pthread_join(ack_get_tid, NULL);
				usleep(100000);
			}

			usleep(500000);
			break;
		}

			// From team
		case CATCH_POKEMON: {
			broker_logger_info("CATCH RECEIVED FROM TEAM");
			t_catch_pokemon *catch_rcv = utils_receive_and_deserialize(
					client_fd, protocol);
			broker_logger_info("Nombre Pokemon: %s", catch_rcv->nombre_pokemon);
			broker_logger_info("Largo nombre: %d", catch_rcv->tamanio_nombre);
			broker_logger_info("Posicion X: %d", catch_rcv->pos_x);
			broker_logger_info("Posicion Y: %d", catch_rcv->pos_y);
			catch_rcv->id_correlacional = generar_id();
			broker_logger_info("ID correlacional: %d",
					catch_rcv->id_correlacional);
			usleep(50000);

			t_message_to_void *message_void = convert_to_void(protocol,
					catch_rcv);
			int from = save_on_memory(message_void);
			broker_logger_info("STARTING POSITION FOR CATCH_POKEMON: %d", from);
			save_node_list_memory(from, message_void->size_message, CATCH_QUEUE,
					catch_rcv->id_correlacional);
			t_catch_pokemon* catch_send = get_from_memory(protocol, from,
					memory);
			catch_send->id_correlacional = catch_rcv->id_correlacional;

			// TODO: Enviar ID a team
			send(client_fd, &catch_rcv->id_correlacional, sizeof(uint32_t), 0);

			create_message_ack(catch_rcv->id_correlacional, catch_queue,
					CATCH_QUEUE);

			// To GC
			catch_protocol = CATCH_POKEMON;
			broker_logger_info("CATCH SENT TO GC");

			_Bool node_matches_catch(t_subscribe_message_node* node) {
				return node->id == catch_send->id_correlacional
						&& node->cola == CATCH_QUEUE;
			}

			void send_msg_to_sub(t_subscribe_nodo* node) {
				_Bool subscriber_ack_matches_catch(t_subscribe_ack_node* n) {
					return n->subscribe->f_desc == node->f_desc;
				}
				utils_serialize_and_send(node->f_desc, catch_protocol,
						catch_send);

				t_protocol rcv_int;
				int r = recv(node->f_desc, &rcv_int, sizeof(t_protocol),
				MSG_WAITALL);
				broker_logger_warn("%d", r);
				broker_logger_warn("ACK Received");

				t_subscribe_message_node* node_ack = list_find(
						list_msg_subscribers, (void*) node_matches_catch);
				t_subscribe_ack_node* node_subscriber = list_find(
						node_ack->list, (void*) subscriber_ack_matches_catch);
				node_subscriber->ack = true;
			}

			for (int i = 0; i < list_size(catch_queue); i++) {
				t_subscribe_nodo* node = list_get(catch_queue, i);

				pthread_t ack_catch_tid;
				pthread_create(&ack_catch_tid, NULL, (void*) send_msg_to_sub,
						(void*) node);
				pthread_detach(ack_catch_tid);
				usleep(100000);
			}
			usleep(500000);
			break;
		}

			// From GC
		case LOCALIZED_POKEMON: {
			broker_logger_info("LOCALIZED RECEIVED FROM GC");
			t_localized_pokemon *loc_rcv = utils_receive_and_deserialize(
					client_fd, protocol);
			broker_logger_info("ID correlacional: %d",
					loc_rcv->id_correlacional);
			broker_logger_info("Nombre Pokemon: %s", loc_rcv->nombre_pokemon);
			broker_logger_info("Largo nombre: %d", loc_rcv->tamanio_nombre);
			broker_logger_info("Cant Elementos en lista: %d",
					loc_rcv->cant_elem);
			for (int el = 0; el < loc_rcv->cant_elem; el++) {
				t_position* pos = malloc(sizeof(t_position));
				pos = list_get(loc_rcv->posiciones, el);
				broker_logger_info("Position is (%d, %d)", pos->pos_x,
						pos->pos_y);
			}
			t_message_to_void *message_void = convert_to_void(protocol,
					loc_rcv);
			int from = save_on_memory(message_void);
			broker_logger_info("STARTING POSITION FOR LOCALIZED_POKEMON: %d",
					from);
			save_node_list_memory(from, message_void->size_message,
					LOCALIZED_QUEUE, loc_rcv->id_correlacional);
			t_localized_pokemon* loc_snd = get_from_memory(protocol, from,
					memory);

			create_message_ack(loc_rcv->id_correlacional, localized_queue,
					LOCALIZED_QUEUE);

			loc_snd->id_correlacional = loc_rcv->id_correlacional;

			// To team
			localized_protocol = LOCALIZED_POKEMON;
			broker_logger_info("LOCALIZED SENT TO TEAM");
			loc_snd->posiciones = loc_rcv->posiciones;

			_Bool node_matches_loc(t_subscribe_message_node* node) {
				return node->id == loc_snd->id_correlacional
						&& node->cola == LOCALIZED_QUEUE;
			}

			void send_msg_to_sub(t_subscribe_nodo* node) {
				_Bool subscriber_ack_matches_loc(t_subscribe_ack_node* n) {
					return n->subscribe->f_desc == node->f_desc;
				}

				usleep(50000);
				utils_serialize_and_send(node->f_desc, localized_protocol, loc_snd);

				t_protocol rcv_int;
				int r = recv(node->f_desc, &rcv_int, sizeof(t_protocol),
				MSG_WAITALL);
				broker_logger_warn("%d", r);
				broker_logger_warn("ACK Received");

				t_subscribe_message_node* node_ack = list_find(
						list_msg_subscribers, (void*) node_matches_loc);
				t_subscribe_ack_node* node_subscriber = list_find(
						node_ack->list, (void*) subscriber_ack_matches_loc);
				node_subscriber->ack = true;
			}

			for (int i = 0; i < list_size(localized_queue); i++) {
				t_subscribe_nodo* node = list_get(localized_queue, i);

				pthread_t ack_loc_tid;
				pthread_create(&ack_loc_tid, NULL, (void*) send_msg_to_sub,
						(void*) node);
				pthread_detach(ack_loc_tid);
				usleep(100000);
			}
			usleep(50000);
			break;
		}

			// From Team or GC
		case SUBSCRIBE: {
			broker_logger_info("SUBSCRIBE RECEIVED");
			t_subscribe *sub_rcv = utils_receive_and_deserialize(client_fd,
					protocol);
			char * ip = string_duplicate(sub_rcv->ip);
			broker_logger_info("Puerto Recibido: %d", sub_rcv->puerto);
			broker_logger_info("IP Recibido: %s", ip);
			sub_rcv->f_desc = client_fd;
			search_queue(sub_rcv);
			usleep(50000);
			break;
		}

			// From GC or GB
		case CAUGHT_POKEMON: {
			broker_logger_info("CAUGHT RECEIVED");
			t_caught_pokemon *caught_rcv = utils_receive_and_deserialize(
					client_fd, protocol);
			broker_logger_info("ID correlacional: %d",
					caught_rcv->id_correlacional);
			broker_logger_info("Resultado (0/1): %d", caught_rcv->result);
			usleep(50000);
			t_message_to_void *message_void = convert_to_void(protocol,
					caught_rcv);
			int from = save_on_memory(message_void);
			broker_logger_info("STARTING POSITION FOR CAUGHT_POKEMON: %d",
					from);
			save_node_list_memory(from, message_void->size_message,
					CAUGHT_QUEUE, caught_rcv->id_correlacional);
			t_caught_pokemon* caught_snd = get_from_memory(protocol, from,
					memory);
			create_message_ack(caught_snd->id_correlacional, caught_queue,
					CAUGHT_QUEUE);

			caught_snd->id_correlacional = caught_rcv->id_correlacional;
			// To Team
			caught_protocol = CAUGHT_POKEMON;
			broker_logger_info("CAUGHT SENT TO TEAM");

			_Bool node_matches_caught(t_subscribe_message_node* node) {
				return node->id == caught_snd->id_correlacional
						&& node->cola == CAUGHT_QUEUE;
			}

			void send_msg_to_sub(t_subscribe_nodo* node) {
				_Bool subscriber_ack_matches_caught(t_subscribe_ack_node* n) {
					return n->subscribe->f_desc == node->f_desc;
				}

				utils_serialize_and_send(node->f_desc, caught_protocol,
						caught_snd);

				t_protocol rcv_int;
				int r = recv(node->f_desc, &rcv_int, sizeof(t_protocol),
				MSG_WAITALL);
				broker_logger_warn("%d", r);
				broker_logger_warn("ACK Received");

				t_subscribe_message_node* node_ack = list_find(
						list_msg_subscribers, (void*) node_matches_caught);
				t_subscribe_ack_node* node_subscriber = list_find(
						node_ack->list, (void*) subscriber_ack_matches_caught);
				node_subscriber->ack = true;
			}

			for (int i = 0; i < list_size(caught_queue); i++) {
				t_subscribe_nodo* node = list_get(caught_queue, i);

				pthread_t ack_caught_tid;
				pthread_create(&ack_caught_tid, NULL, (void*) send_msg_to_sub,
						(void*) node);
				pthread_detach(ack_caught_tid);
				usleep(100000);
			}
			usleep(50000);
			break;
		}

		default:
			break;
		}
	}
}

void initialize_queue() {
	id = 0;
	get_queue = list_create();
	appeared_queue = list_create();
	new_queue = list_create();
	caught_queue = list_create();
	catch_queue = list_create();
	localized_queue = list_create();
	list_memory = list_create();
	list_msg_subscribers = list_create();
}

t_subscribe_nodo* check_already_subscribed(char *ip, uint32_t puerto,
		t_list *list) {
	int find_subscribed(t_subscribe_nodo *nodo) {
		return (strcmp(ip, nodo->ip) == 0 && (puerto == nodo->puerto));
	}

	return list_find(list, (void*) find_subscribed);
}

void remove_after_n_secs(t_subscribe_nodo* sub, t_list* q, int n) {
	_Bool is_gb_subscriber(t_subscribe_nodo* sub) {
		return sub->endtime != -1;
	}

	int sub_time = (int) time(NULL);
	for (;;) {
		if ((int) time(NULL) > (sub_time + n)) {

			// TODO: Sync!
			t_empty* noop = malloc(sizeof(t_empty));
			t_protocol noop_protocol = NOOP;
			utils_serialize_and_send(sub->f_desc, noop_protocol, noop);

			list_remove_by_condition(q, (void*) is_gb_subscriber);
			broker_logger_info(
					"Game boy has been kicked from subscribers list");
			return;
		}
		usleep((n / 2) * 100000);
	}
}

void add_to(t_list *list, t_subscribe* subscriber) {

	t_subscribe_nodo* node = check_already_subscribed(subscriber->ip,
			subscriber->puerto, list);
	if (node == NULL) {
		t_subscribe_nodo *nodo = malloc(sizeof(t_subscribe_nodo));
		nodo->ip = string_duplicate(subscriber->ip);
		nodo->puerto = subscriber->puerto;
		// Sync uid_subscribe var
		nodo->id = uid_subscribe;
		uid_subscribe++;

		void broker_handle_removal() {
			remove_after_n_secs(nodo, list, subscriber->seconds);
		}

		if (subscriber->proceso == GAME_BOY) {
			nodo->endtime = time(NULL) + subscriber->seconds;
		} else {
			nodo->endtime = -1;
		}

		nodo->f_desc = subscriber->f_desc;
		list_add(list, nodo);

		if (nodo->endtime != -1) {
			pthread_t sub_tid;
			pthread_create(&sub_tid, NULL, (void*) broker_handle_removal, NULL);
			pthread_detach(sub_tid);
			usleep(100000);
		}
	} else {
		broker_logger_info("Ya esta suscripto");
		node->f_desc = subscriber->f_desc;
	}
}

void search_queue(t_subscribe *subscriber) {

	switch (subscriber->cola) {
	case NEW_QUEUE: {
		broker_logger_info("Suscripto IP: %s, PUERTO: %d,  a Cola NEW ",
				subscriber->ip, subscriber->puerto);
		add_to(new_queue, subscriber);
		send_all_messages(subscriber);
		break;
	}
	case CATCH_QUEUE: {
		broker_logger_info("Suscripto IP: %s, PUERTO: %d,  a Cola CATCH ",
				subscriber->ip, subscriber->puerto);
		add_to(catch_queue, subscriber);
		send_all_messages(subscriber);
		break;
	}
	case CAUGHT_QUEUE: {
		broker_logger_info("Suscripto IP: %s, PUERTO: %d,  a Cola CAUGHT ",
				subscriber->ip, subscriber->puerto);
		add_to(caught_queue, subscriber);
		send_all_messages(subscriber);
		break;
	}
	case GET_QUEUE: {
		broker_logger_info("Suscripto IP: %s, PUERTO: %d,  a Cola GET ",
				subscriber->ip, subscriber->puerto);
		add_to(get_queue, subscriber);
		send_all_messages(subscriber);
		break;
	}
	case LOCALIZED_QUEUE: {
		broker_logger_info("Suscripto IP: %s, PUERTO: %d,  a Cola LOCALIZED ",
				subscriber->ip, subscriber->puerto);
		add_to(localized_queue, subscriber);
		send_all_messages(subscriber);
		break;
	}
	case APPEARED_QUEUE: {
		broker_logger_info("Suscripto IP: %s, PUERTO: %d,  a Cola APPEARED ",
				subscriber->ip, subscriber->puerto);
		add_to(appeared_queue, subscriber);
		send_all_messages(subscriber);
		break;
	}

	}

}

void broker_exit() {
	socket_close_conection(broker_socket);
	broker_config_free();
	broker_logger_destroy();
}

t_message_to_void *convert_to_void(t_protocol protocol, void *package_recv) {

	int offset = 0;
	t_message_to_void* message_to_void = malloc(sizeof(t_message_to_void));
	message_to_void->size_message = 0;
	switch (protocol) {
	case NOOP: {
		break;
	}
	case ACK: {
		break;
	}
	case SUBSCRIBE: {
		break;
	}
	case HANDSHAKE: {
		break;
	}
	case NEW_POKEMON: {
		broker_logger_info("NEW RECEIVED, CONVERTING to VOID*..");
		t_new_pokemon *new_receive = (t_new_pokemon*) package_recv;
		message_to_void->message = malloc(
				new_receive->tamanio_nombre + (sizeof(uint32_t) * 4));
		memcpy(message_to_void->message + offset, &new_receive->tamanio_nombre,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(message_to_void->message + offset, new_receive->nombre_pokemon,
				new_receive->tamanio_nombre);
		offset += new_receive->tamanio_nombre;
		memcpy(message_to_void->message + offset, &new_receive->pos_x,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(message_to_void->message + offset, &new_receive->pos_y,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(message_to_void->message + offset, &new_receive->cantidad,
				sizeof(uint32_t));
		message_to_void->size_message = new_receive->tamanio_nombre
				+ (sizeof(uint32_t) * 4);

		break;
	}

		// From GB or GC
	case APPEARED_POKEMON: {
		broker_logger_info("APPEARED RECEIVED, CONVERTING TO VOID*..");

		t_appeared_pokemon *appeared_rcv = (t_appeared_pokemon*) package_recv;
		message_to_void->message = malloc(
				appeared_rcv->tamanio_nombre + sizeof(uint32_t) * 3);

		memcpy(message_to_void->message + offset, &appeared_rcv->tamanio_nombre,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(message_to_void->message + offset, appeared_rcv->nombre_pokemon,
				appeared_rcv->tamanio_nombre);
		offset += appeared_rcv->tamanio_nombre;
		memcpy(message_to_void->message + offset, &appeared_rcv->pos_x,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(message_to_void->message + offset, &appeared_rcv->pos_y,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(message_to_void->message + offset, &appeared_rcv->cantidad,
				sizeof(uint32_t));

		message_to_void->size_message = appeared_rcv->tamanio_nombre
				+ sizeof(uint32_t) * 4;
		break;
	}
		// From team
	case GET_POKEMON: {
		broker_logger_info("GET RECEIVED, CONVERTING TO VOID*..");
		t_get_pokemon *get_rcv = (t_get_pokemon*) package_recv;
		message_to_void->message = malloc(
				get_rcv->tamanio_nombre + sizeof(uint32_t));

		memcpy(message_to_void->message + offset, &get_rcv->tamanio_nombre,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(message_to_void->message + offset, get_rcv->nombre_pokemon,
				get_rcv->tamanio_nombre);
		message_to_void->size_message = get_rcv->tamanio_nombre
				+ sizeof(uint32_t);
		break;
	}

		// From team
	case CATCH_POKEMON: {
		broker_logger_info("CATCH RECEIVED, CONVERTING TO VOID*..");
		t_catch_pokemon *catch_rcv = (t_catch_pokemon*) package_recv;
		message_to_void->message = malloc(
				catch_rcv->tamanio_nombre + sizeof(uint32_t) * 3);

		memcpy(message_to_void->message + offset, &catch_rcv->tamanio_nombre,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(message_to_void->message + offset, catch_rcv->nombre_pokemon,
				catch_rcv->tamanio_nombre);
		offset += catch_rcv->tamanio_nombre;
		memcpy(message_to_void->message + offset, &catch_rcv->pos_x,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(message_to_void->message + offset, &catch_rcv->pos_y,
				sizeof(uint32_t));

		message_to_void->size_message = catch_rcv->tamanio_nombre
				+ sizeof(uint32_t) * 3;
		break;
	}
		// From GC
	case LOCALIZED_POKEMON: {
		broker_logger_info("LOCALIZED RECEIVED, CONVERTING TO VOID*..");
		t_localized_pokemon *loc_rcv = (t_localized_pokemon*) package_recv;
		message_to_void->message = malloc(
				loc_rcv->tamanio_nombre + sizeof(uint32_t) + sizeof(uint32_t)
						+ sizeof(uint32_t) * 2 * loc_rcv->cant_elem);

		memcpy(message_to_void->message + offset, &loc_rcv->tamanio_nombre,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(message_to_void->message + offset, loc_rcv->nombre_pokemon,
				loc_rcv->tamanio_nombre);
		offset += loc_rcv->tamanio_nombre;
		memcpy(message_to_void->message + offset, &loc_rcv->cant_elem,
				sizeof(uint32_t));
		for (int i = 0; i < loc_rcv->cant_elem; i++) {
			offset += sizeof(uint32_t);
			t_position *pos = list_get(loc_rcv->posiciones, i);
			memcpy(message_to_void->message + offset, &pos->pos_x,
					sizeof(uint32_t));
			offset += sizeof(uint32_t);
			memcpy(message_to_void->message + offset, &pos->pos_y,
					sizeof(uint32_t));

		}
		message_to_void->size_message = loc_rcv->tamanio_nombre
				+ sizeof(uint32_t) + sizeof(uint32_t)
				+ sizeof(uint32_t) * 2 * loc_rcv->cant_elem;
		break;
	}

	case CAUGHT_POKEMON: {
		broker_logger_info("CAUGHT RECEIVED, CONVERTING TO VOID*..");
		t_caught_pokemon *caught_rcv = (t_caught_pokemon*) package_recv;
		message_to_void->message = malloc(sizeof(uint32_t));

		memcpy(message_to_void->message, &caught_rcv->result, sizeof(uint32_t));
		message_to_void->size_message = sizeof(uint32_t);
		break;
	}

	}
	return message_to_void;

}

void *get_from_memory(t_protocol protocol, int posicion, void *message) {

	int offset = posicion;

	switch (protocol) {
	case NOOP: {
		break;
	}
	case ACK: {
		break;
	}
	case SUBSCRIBE: {
		break;
	}
	case HANDSHAKE: {
		break;
	}
	case NEW_POKEMON: {
		broker_logger_info("GETTING \"NEW\" MESSAGE FROM  MEMORY..");
		t_new_pokemon *new_receive = malloc(sizeof(t_new_pokemon));

		memcpy(&new_receive->tamanio_nombre, message + offset,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);
		new_receive->nombre_pokemon = malloc(new_receive->tamanio_nombre);
		memset(new_receive->nombre_pokemon, 0, new_receive->tamanio_nombre + 1);
		memcpy(new_receive->nombre_pokemon, message + offset,
				new_receive->tamanio_nombre);
		offset += new_receive->tamanio_nombre;

		memcpy(&new_receive->pos_x, message + offset, sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(&new_receive->pos_y, message + offset, sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(&new_receive->cantidad, message + offset, sizeof(uint32_t));

		broker_logger_info("******************************************");
		broker_logger_info("RECEIVED:");
		broker_logger_info("Pokemon: %s", new_receive->nombre_pokemon);
		broker_logger_info("Name length: %d", new_receive->tamanio_nombre);
		broker_logger_info("X Axis position: %d", new_receive->pos_x);
		broker_logger_info("Y Axis position: %d", new_receive->pos_y);
		broker_logger_info("Quantity: %d", new_receive->cantidad);

		return new_receive;

	}

		// From GB or GC
	case APPEARED_POKEMON: {
		broker_logger_info("GETTING \"APPEARED\" MESSAGE FROM  MEMORY..");

		t_appeared_pokemon *appeared_rcv = malloc(sizeof(t_appeared_pokemon));

		memcpy(&appeared_rcv->tamanio_nombre, message + offset,
				sizeof(uint32_t));
		offset += sizeof(uint32_t);

		appeared_rcv->nombre_pokemon = malloc(appeared_rcv->tamanio_nombre);
		memset(appeared_rcv->nombre_pokemon, 0,
				appeared_rcv->tamanio_nombre + 1);
		memcpy(appeared_rcv->nombre_pokemon, message + offset,
				appeared_rcv->tamanio_nombre);

		offset += appeared_rcv->tamanio_nombre;
		memcpy(&appeared_rcv->pos_x, message + offset, sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(&appeared_rcv->pos_y, message + offset, sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(&appeared_rcv->cantidad, message + offset, sizeof(uint32_t));

		broker_logger_info("******************************************");
		broker_logger_info("RECEIVED:");
		broker_logger_info("Pokemon: %s", appeared_rcv->nombre_pokemon);
		broker_logger_info("Name length: %d", appeared_rcv->tamanio_nombre);
		broker_logger_info("X Axis position: %d", appeared_rcv->pos_x);
		broker_logger_info("Y Axis position: %d", appeared_rcv->pos_y);
		broker_logger_info("Quantity: %d", appeared_rcv->cantidad);

		return appeared_rcv;
	}
		// From team
	case GET_POKEMON: {
		broker_logger_info("GETING \"GET\" MESSAGE FROM  MEMORY..");
		t_get_pokemon* get_rcv = malloc(sizeof(t_get_pokemon));

		memcpy(&get_rcv->tamanio_nombre, message + offset, sizeof(uint32_t));
		offset += sizeof(uint32_t);
		get_rcv->nombre_pokemon = malloc(get_rcv->tamanio_nombre);
		memset(get_rcv->nombre_pokemon, 0, get_rcv->tamanio_nombre + 1);
		memcpy(get_rcv->nombre_pokemon, message + offset,
				get_rcv->tamanio_nombre);

		broker_logger_info("******************************************");
		broker_logger_info("RECEIVED:");
		broker_logger_info("Pokemon: %s", get_rcv->nombre_pokemon);
		broker_logger_info("Name length: %d", get_rcv->tamanio_nombre);

		return get_rcv;
	}

		// From team
	case CATCH_POKEMON: {
		broker_logger_info("GETTING \"CATCH\" MESSAGE FROM  MEMORY..");
		t_catch_pokemon *catch_rcv = malloc(sizeof(t_catch_pokemon));
		memcpy(&catch_rcv->tamanio_nombre, message + offset, sizeof(uint32_t));
		offset += sizeof(uint32_t);
		catch_rcv->nombre_pokemon = malloc(catch_rcv->tamanio_nombre);
		memset(catch_rcv->nombre_pokemon, 0, catch_rcv->tamanio_nombre + 1);
		memcpy(catch_rcv->nombre_pokemon, message + offset,
				catch_rcv->tamanio_nombre);
		offset += catch_rcv->tamanio_nombre;
		memcpy(&catch_rcv->pos_x, message + offset, sizeof(uint32_t));
		offset += sizeof(uint32_t);
		memcpy(&catch_rcv->pos_y, message + offset, sizeof(uint32_t));

		broker_logger_info("******************************************");
		broker_logger_info("RECEIVED:");
		broker_logger_info("Pokemon: %s", catch_rcv->nombre_pokemon);
		broker_logger_info("Name length: %d", catch_rcv->tamanio_nombre);
		broker_logger_info("X Position: %d", catch_rcv->pos_x);
		broker_logger_info("Y Position: %d", catch_rcv->pos_y);

		return catch_rcv;
	}
		// From GC
	case LOCALIZED_POKEMON: {
		broker_logger_info("GETTING \"LOCALIZED\" MESSAGE FROM MEMORY..");
		t_localized_pokemon *loc_rcv = malloc(sizeof(t_localized_pokemon));

		memcpy(&loc_rcv->tamanio_nombre, message + offset, sizeof(uint32_t));
		offset += sizeof(uint32_t);
		loc_rcv->nombre_pokemon = malloc(loc_rcv->tamanio_nombre);
		memset(loc_rcv->nombre_pokemon, 0, loc_rcv->tamanio_nombre + 1);
		memcpy(loc_rcv->nombre_pokemon, message + offset,
				loc_rcv->tamanio_nombre);

		loc_rcv->posiciones = list_create();
		offset += loc_rcv->tamanio_nombre;
		memcpy(&loc_rcv->cant_elem, message + offset, sizeof(uint32_t));

		broker_logger_info("******************************************");
		broker_logger_info("RECEIVED:");
		broker_logger_info("Pokemon: %s", loc_rcv->nombre_pokemon);
		broker_logger_info("Name length: %d", loc_rcv->tamanio_nombre);
		broker_logger_info("Quantity: %d", loc_rcv->cant_elem);
		for (int i = 0; i < loc_rcv->cant_elem; i++) {
			offset += sizeof(uint32_t);
			t_position* pos = malloc(sizeof(t_position));
			memcpy(&pos->pos_x, message + offset, sizeof(uint32_t));
			offset += sizeof(uint32_t);
			memcpy(&pos->pos_y, message + offset, sizeof(uint32_t));
			list_add(loc_rcv->posiciones, pos);
			broker_logger_info("X Axis Position: %d", pos->pos_x);
			broker_logger_info("Y Axis Position: %d", pos->pos_y);

		}
		return loc_rcv;
	}

	case CAUGHT_POKEMON: {
		broker_logger_info("GETTING \"CAUGHT\" MESSAGE FROM MEMORY..");
		t_caught_pokemon *caught_rcv = malloc(sizeof(t_caught_pokemon));

		broker_logger_info("******************************************");
		broker_logger_info("RECEIVED:");
		memcpy(&caught_rcv->result, message, sizeof(uint32_t));
		if (caught_rcv->result) {
			broker_logger_info("Result: Caught");
		} else {
			broker_logger_info("Result: Failed");
		}

		return caught_rcv;
	}
	}
	return NULL;

}

_Bool is_buddy() {
	return broker_config->estrategia_memoria == 0;
}

int compare_timings(const void* a, const void* b) {
	return (((t_nodo_memory*) a)->timestamp > ((t_nodo_memory*) b)->timestamp) ?
			-1 : 1;
}

int compare_memory_position(const void* a, const void* b) {
	return (((t_nodo_memory*) a)->pointer > ((t_nodo_memory*) b)->pointer) ?
			-1 : 1;
}

void update_timings(t_nodo_memory* node) {
	if (broker_config->algoritmo_reemplazo == 1) {
		time(&node->timestamp);
	}

	else
		return;
}

char* get_queue_name(t_cola q) {

	char* out = string_duplicate("");

	switch (q) {

	case NEW_QUEUE:
		out = "NEW_QUEUE";
		break;

	case LOCALIZED_QUEUE:
		out = "LOCALIZED_QUEUE";
		break;

	case CATCH_QUEUE:
		out = "CATCH_QUEUE";
		break;

	case CAUGHT_QUEUE:
		out = "CAUGHT_QUEUE";
		break;

	case APPEARED_QUEUE:
		out = "APPEARED_QUEUE";
		break;

	case GET_QUEUE:
		out = "GET_QUEUE";
		break;
	}
	return out;
}

void purge_msg() {

	list_sort(list_memory, (void*) compare_timings);

	/**
	 * For debugging purposes, will be removed afterwards
	 * for (int i=0; i< list_memory->elements_count; i++) {
	 * t_nodo_memory* nd = (t_nodo_memory*)list_get(list_memory, i);
	 * broker_logger_warn("%.2f", (float) nd->timestamp);
	 * }
	 *
	 **/

	t_nodo_memory* node = (t_nodo_memory*) list_get(list_memory, 0);
	broker_logger_warn(
			"The message with ID %d from %s, last modified at instant T=%d will be removed",
			node->id, get_queue_name(node->cola),
			(int) (node->timestamp - base_time));
	list_remove(list_memory, 0);

	buddy_free(buddy, node->pointer);
	broker_logger_info("Message removed successfully");
	broker_logger_info("Memory consolidated");
}

int save_on_memory(t_message_to_void *message_void) {
	pthread_mutex_lock(&mpointer);
	int from =
			is_buddy() ?
					buddy_alloc(buddy, message_void->size_message) : pointer;

	if (!is_buddy()) {
		if (message_void->size_message
				< broker_config->tamano_minimo_particion) {
			pointer += broker_config->tamano_minimo_particion;
		}

		else {
			pointer += message_void->size_message;
		}
	}

	memcpy(memory + from, message_void->message, message_void->size_message);
	pthread_mutex_unlock(&mpointer);
	return from;
}

void save_node_list_memory(int pointer, int msg_size, t_cola cola, int id) {
	t_nodo_memory * nodo_mem = malloc(sizeof(t_nodo_memory));

	nodo_mem->pointer = pointer;

	nodo_mem->size =
			!is_buddy() ?
					((msg_size < broker_config->tamano_minimo_particion) ?
							broker_config->tamano_minimo_particion : msg_size) :
					msg_size;

	nodo_mem->cola = cola;
	nodo_mem->id = id;
	nodo_mem->timestamp = time(NULL);
	list_add(list_memory, nodo_mem);
}

t_nodo_memory* find_node(t_nodo_memory* node) {

	int ret_pos = 0;

	for (int i = 0; i < list_size(list_memory); i++) {
		t_nodo_memory* nodo_mem = list_get(list_memory, i);
		int id_nodo_mem = nodo_mem->id;
		t_cola queue_nodo_mem = nodo_mem->cola;

		if (node->id == id_nodo_mem && node->cola == queue_nodo_mem) {
			ret_pos = i;
			break;
		}
	}

	return list_get(list_memory, ret_pos);
}

void send_all_messages(t_subscribe *subscriber) {
	_Bool msg_match(t_nodo_memory *node) {
		return node->cola == subscriber->cola;
	}

	t_list* list_cpy = list_filter(list_memory, (void*) msg_match);
	int cant = list_size(list_cpy);
	for (int i = 0; i < cant; i++) {
		t_nodo_memory *nodo_cpy = list_get(list_cpy, i);

		t_nodo_memory *nodo_mem = find_node(nodo_cpy);
		update_timings(nodo_mem);

		switch (subscriber->cola) {
		case NEW_QUEUE: {
			t_new_pokemon* new_snd = get_from_memory(NEW_POKEMON,
					nodo_mem->pointer, memory);
			new_snd->id_correlacional = nodo_mem->id;
			utils_serialize_and_send(subscriber->f_desc, NEW_POKEMON, new_snd);
			break;
		}
		case CATCH_QUEUE: {
			t_catch_pokemon* catch_snd = get_from_memory(CATCH_POKEMON,
					nodo_mem->pointer, memory);
			catch_snd->id_correlacional = nodo_mem->id;
			utils_serialize_and_send(subscriber->f_desc, CATCH_POKEMON,
					catch_snd);
			break;
		}
		case CAUGHT_QUEUE: {
			t_caught_pokemon* caught_snd = get_from_memory(CAUGHT_POKEMON,
					nodo_mem->pointer, memory);
			caught_snd->id_correlacional = nodo_mem->id;
			utils_serialize_and_send(subscriber->f_desc, CAUGHT_POKEMON,
					caught_snd);
			break;
		}
		case GET_QUEUE: {
			t_get_pokemon* get_snd = get_from_memory(GET_POKEMON,
					nodo_mem->pointer, memory);
			get_snd->id_correlacional = nodo_mem->id;
			utils_serialize_and_send(subscriber->f_desc, GET_POKEMON, get_snd);
			break;
		}
		case LOCALIZED_QUEUE: {
			t_localized_pokemon* localized_snd = get_from_memory(
					LOCALIZED_POKEMON, nodo_mem->pointer, memory);
			localized_snd->id_correlacional = nodo_mem->id;
			utils_serialize_and_send(subscriber->f_desc, LOCALIZED_POKEMON,
					localized_snd);
			break;
		}
		case APPEARED_QUEUE: {
			t_appeared_pokemon* appeared_snd = get_from_memory(APPEARED_POKEMON,
					nodo_mem->pointer, memory);
			appeared_snd->id_correlacional = nodo_mem->id;
			utils_serialize_and_send(subscriber->f_desc, APPEARED_POKEMON,
					appeared_snd);
			break;
		}

		}

	}
}

void create_message_ack(int id, t_list *cola, t_cola unCola) {
	t_subscribe_message_node* message_node = malloc(
			sizeof(t_subscribe_message_node));
	message_node->id = id;
	message_node->cola = unCola;
	message_node->list = list_create();
	list_add(list_msg_subscribers, message_node);

	for (int i = 0; i < list_size(cola); i++) {
		t_subscribe_nodo* subscriptor = list_get(cola, i);
		t_subscribe_ack_node* ack_subscriptor = malloc(
				sizeof(t_subscribe_ack_node));
		ack_subscriptor->subscribe = subscriptor;
		ack_subscriptor->ack = false;
		list_add(message_node->list, ack_subscriptor);
	}
}

void dump() {

	int last_size = 0;
	int last_pointer = 0;

	FILE *f = NULL;
	f = fopen("memdump.txt", "a");

	if (f == NULL) {
		broker_logger_error("Operation failed: Couldn't dump memory contents");
		return;
	}
	if (ftell(f) != 0) {
		fprintf(f,
				"------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------\n");
	}
	time_t _time = time(NULL);
	struct tm *tm = localtime(&_time);
	char s[64];
	strftime(s, sizeof(s), "%c", tm);
	fprintf(f, "Dump %s\n", s);
	t_list* list_clone = list_duplicate(list_memory);
	list_sort(list_clone, (void*) compare_memory_position);

	for (int i = 0; i < list_size(list_clone); ++i) {

		t_nodo_memory* node = list_get(list_clone, i);

		// If first elem is free
		if (i == 0 && node->pointer != 0) {
			fprintf(f, "Particion %04d: %04d - %04d\t\t", i + 1, 0,
					(node->pointer - 1));
			fprintf(f, "[L]\t\t");
			fprintf(f, "Size: %04d B\n", node->pointer);
			fprintf(f, "Particion %04d: %04d - %04d\t\t", i + 2, node->pointer,
					next_power_of_2(node->pointer + (node->size)) - 1);
			fprintf(f, "[X]\t\t");
			fprintf(f, "Total size: %04d B. Used: %04d B, Free: %04d B\t\t",
					next_power_of_2(node->size), node->size,
					next_power_of_2(node->size) - node->size);
			fprintf(f, "LRU: %04d\t\t", (int) (node->timestamp - base_time));
			fprintf(f, "Queue: %s\t\t", get_queue_name(node->cola));
			fprintf(f, "ID: %04d\n", node->id);
			last_pointer = next_power_of_2(node->pointer + node->size);
			last_size = i + 2;
		}

		else {
			// If there's a hole in the middle
			if (node->pointer > last_pointer) {
				fprintf(f, "Particion %04d: %04d - %04d\t\t", last_size + 1,
						last_pointer, node->pointer - 1);
				fprintf(f, "[L]\t\t");
				fprintf(f, "Size: %04d B\n", node->pointer - last_pointer);
				fprintf(f, "Particion %04d: %04d - %04d\t\t", last_size + 2,
						node->pointer, next_power_of_2(node->size) - 1);
				fprintf(f, "[X]\t\t");
				fprintf(f, "Total size: %04d B. Used: %04d B, Free: %04d B\t\t",
						next_power_of_2(node->size), node->size,
						next_power_of_2(node->size) - node->size);
				fprintf(f, "LRU: %04d\t\t",
						(int) (node->timestamp - base_time));
				fprintf(f, "Queue: %s\t\t", get_queue_name(node->cola));
				fprintf(f, "ID: %04d\n", node->id);
				last_size += 2;
			}

			else {
				fprintf(f, "Particion %04d: %04d - %04d\t\t", last_size + 1,
						node->pointer,
						next_power_of_2(node->pointer + (node->size)) - 1);
				fprintf(f, "[X]\t\t");
				fprintf(f, "Total size: %04d B. Used: %04d B, Free: %04d B\t\t",
						next_power_of_2(node->size), node->size,
						next_power_of_2(node->size) - node->size);
				fprintf(f, "LRU: %04d\t\t",
						(int) (node->timestamp - base_time));
				fprintf(f, "Queue: %s\t\t", get_queue_name(node->cola));
				fprintf(f, "ID: %04d\n", node->id);
				last_size += 1;
			}
			last_pointer = next_power_of_2(node->pointer + node->size);
		}

		// if last block is empty
		if ((i == (list_size(list_clone) - 1))
				&& last_pointer != broker_config->tamano_memoria) {
			fprintf(f, "Particion %d: %d - %d\t\t", last_size + 1, last_pointer,
					broker_config->tamano_memoria - 1);
			fprintf(f, "[L]\t\t");
			fprintf(f, "Size: %d B\n",
					broker_config->tamano_memoria - last_pointer);
			last_size += 1;
		}
	}
	fclose(f);
	return;
}

int generar_id() {
	pthread_mutex_lock(&mid);
	id++;
	pthread_mutex_unlock(&mid);
	return id;
}

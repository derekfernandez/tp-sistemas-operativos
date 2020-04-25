#include "broker.h"

int main(int argc, char *argv[]) {
	initialize_queue();
	if (broker_load() < 0)
		return EXIT_FAILURE;
	broker_server_init();
	broker_exit();

	return EXIT_SUCCESS;
}

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

	return 0;
}

void broker_server_init() {

	broker_socket = socket_create_listener(broker_config->ip_broker,
			broker_config->puerto_broker);
	if (broker_socket < 0) {
		broker_logger_error("Error al crear server");
		return;
	}

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
	uint32_t uuid = 0000;

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
			broker_logger_error("Error al recibir mensaje");
			return NULL;
		}
		switch (protocol) {

		case ACK: {
			broker_logger_info("Ack received");
			t_ack *ack_receive = utils_receive_and_deserialize(client_fd,
					protocol);
			broker_logger_info("ID recibido: %d", ack_receive->id);
			broker_logger_info("ID correlacional %d",
					ack_receive->id_correlacional);
			usleep(100000);
			break;
		}

			// From GB
		case NEW_POKEMON: {
			broker_logger_info("New received");
			t_new_pokemon *new_receive = utils_receive_and_deserialize(
					client_fd, protocol);
			broker_logger_info("ID recibido: %d", new_receive->id);
			broker_logger_info("ID Correlacional: %d",
					new_receive->id_correlacional);
			broker_logger_info("Cantidad: %d", new_receive->cantidad);
			broker_logger_info("Nombre Pokemon: %s",
					new_receive->nombre_pokemon);
			broker_logger_info("Largo Nombre: %d", new_receive->tamanio_nombre);
			broker_logger_info("Posicion X: %d", new_receive->pos_x);
			broker_logger_info("Posicion Y: %d", new_receive->pos_y);
			usleep(100000);

			// To GC
			t_new_pokemon* new_snd = malloc(sizeof(t_new_pokemon));
			new_snd->nombre_pokemon = string_duplicate(
					new_receive->nombre_pokemon);
			new_snd->id = 28;
			new_snd->id_correlacional = new_receive->id_correlacional;
			new_snd->cantidad = new_receive->cantidad;
			new_snd->tamanio_nombre = strlen(new_snd->nombre_pokemon) + 1;
			new_snd->pos_x = new_receive->pos_x;
			new_snd->pos_y = new_receive->pos_y;
			new_protocol = NEW_POKEMON;
			broker_logger_info("NEW SENT TO GC");
			for (int i = 0; i < list_size(new_queue); i++) {
				t_subscribe_nodo* node = list_get(new_queue, i);
				utils_serialize_and_send(node->f_desc, new_protocol, new_snd);
			}

			usleep(50000);
			break;
		}

			// From GB or GC
		case APPEARED_POKEMON: {
			broker_logger_info("Appeared received");
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

			// To Team
			t_appeared_pokemon* appeared_snd = malloc(
					sizeof(t_appeared_pokemon));
			appeared_protocol = APPEARED_POKEMON;
			appeared_snd->nombre_pokemon = appeared_rcv->nombre_pokemon;
			appeared_snd->tamanio_nombre = appeared_rcv->tamanio_nombre;
			appeared_snd->id_correlacional = appeared_rcv->id_correlacional;
			appeared_snd->pos_x = appeared_rcv->pos_x;
			appeared_snd->pos_y = appeared_rcv->pos_y;
			appeared_snd->cantidad = appeared_rcv->cantidad;
			broker_logger_info("APPEARED SENT TO TEAM");
			for (int i = 0; i < list_size(appeared_queue); i++) {
				t_subscribe_nodo* node = list_get(appeared_queue, i);
				utils_serialize_and_send(node->f_desc, appeared_protocol,
						appeared_snd);
			}
			usleep(500000);
			break;
		}
			// From team
		case GET_POKEMON: {
			broker_logger_info("Get received");
			t_get_pokemon *get_rcv = utils_receive_and_deserialize(client_fd,
					protocol);
			broker_logger_info("ID correlacional: %d",
					get_rcv->id_correlacional);
			broker_logger_info("Nombre Pokemon: %s", get_rcv->nombre_pokemon);
			broker_logger_info("Largo nombre: %d", get_rcv->tamanio_nombre);

			usleep(50000);

			// To GC
			t_get_pokemon* get_snd = malloc(sizeof(t_get_pokemon));
			get_snd->id_correlacional = get_rcv->id_correlacional;
			get_snd->nombre_pokemon = get_rcv->nombre_pokemon;
			get_snd->tamanio_nombre = strlen(get_snd->nombre_pokemon) + 1;
			get_protocol = GET_POKEMON;
			broker_logger_info("GET SENT TO GAMECARD");
			for (int i = 0; i < list_size(get_queue); i++) {
				t_subscribe_nodo* node = list_get(get_queue, i);
				utils_serialize_and_send(node->f_desc, get_protocol, get_snd);
			}

			usleep(500000);
			break;
		}

			// From team
		case CATCH_POKEMON: {
			broker_logger_info("Catch received");
			t_catch_pokemon *catch_rcv = utils_receive_and_deserialize(
					client_fd, protocol);
			broker_logger_info("ID correlacional: %d",
					catch_rcv->id_correlacional);
			broker_logger_info("ID Generado: %d", catch_rcv->id_gen);
			broker_logger_info("Nombre Pokemon: %s", catch_rcv->nombre_pokemon);
			broker_logger_info("Largo nombre: %d", catch_rcv->tamanio_nombre);
			broker_logger_info("Posicion X: %d", catch_rcv->pos_x);
			broker_logger_info("Posicion Y: %d", catch_rcv->pos_y);
			usleep(50000);

			// To GC
			t_catch_pokemon* catch_send = malloc(sizeof(t_catch_pokemon));
			catch_send->id_correlacional = catch_rcv->id_correlacional;
			catch_send->nombre_pokemon = catch_rcv->nombre_pokemon;
			catch_send->pos_x = catch_rcv->pos_x;
			catch_send->pos_y = catch_rcv->pos_y;
			catch_send->tamanio_nombre = strlen(catch_rcv->nombre_pokemon) + 1;
			catch_send->id_gen = uuid;
			uuid++;
			catch_protocol = CATCH_POKEMON;
			broker_logger_info("Catch sent");
			for (int i = 0; i < list_size(catch_queue); i++) {
				t_subscribe_nodo* node = list_get(catch_queue, i);
				utils_serialize_and_send(node->f_desc, catch_protocol,
						catch_send);
			}

			usleep(500000);
			break;
		}
			// From GC
		case LOCALIZED_POKEMON: {
			broker_logger_info("Localized received");
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

			// To team
			t_localized_pokemon* loc_snd = malloc(sizeof(t_localized_pokemon));
			loc_snd->id_correlacional = loc_rcv->id_correlacional;
			loc_snd->nombre_pokemon = loc_rcv->nombre_pokemon;
			loc_snd->tamanio_nombre = strlen(loc_snd->nombre_pokemon) + 1;
			loc_snd->cant_elem = list_size(loc_rcv->posiciones);
			localized_protocol = LOCALIZED_POKEMON;
			broker_logger_info("LOCALIZED SENT TO TEAM");
			loc_snd->posiciones = loc_rcv->posiciones;
			for (int i = 0; i < list_size(localized_queue); i++) {
				t_subscribe_nodo* node = list_get(localized_queue, i);
				utils_serialize_and_send(node->f_desc, localized_protocol,
						loc_snd);
			}

			usleep(50000);
			break;
		}

			// From Team or GC
		case SUBSCRIBE: {
			broker_logger_info("SUBSCRIBE received");
			t_subscribe *sub_rcv = utils_receive_and_deserialize(client_fd,
					protocol);
			char * ip = string_duplicate(sub_rcv->ip);
			broker_logger_info("Puerto Recibido: %d", sub_rcv->puerto);
			broker_logger_info("IP Recibido: %s", ip);
			sub_rcv->f_desc = client_fd;
			// TODO: Check dictionary
			search_queue(sub_rcv);
			free(sub_rcv->ip);
			free(sub_rcv);
			usleep(50000);
			break;
		}

			// From GC or GB
		case CAUGHT_POKEMON: {
			broker_logger_info("Caught received");
			t_caught_pokemon *caught_rcv = utils_receive_and_deserialize(
					client_fd, protocol);
			broker_logger_info("ID correlacional: %d",
					caught_rcv->id_correlacional);
			broker_logger_info("ID mensaje: %d", caught_rcv->id_msg);
			broker_logger_info("Resultado (0/1): %d", caught_rcv->result);
			usleep(50000);

			// To Team
			t_caught_pokemon* caught_snd = malloc(sizeof(t_caught_pokemon));
			caught_snd->id_correlacional = caught_rcv->id_correlacional;
			caught_snd->id_msg = caught_rcv->id_msg;
			caught_snd->result = caught_rcv->result;
			caught_protocol = CAUGHT_POKEMON;
			broker_logger_info("CAUGHT SENT TO TEAM");
			for (int i = 0; i < list_size(caught_queue); i++) {
				t_subscribe_nodo* node = list_get(caught_queue, i);
				utils_serialize_and_send(node->f_desc, caught_protocol,
						caught_snd);
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
	get_queue = list_create();
	appeared_queue = list_create();
	new_queue = list_create();
	caught_queue = list_create();
	catch_queue = list_create();
	localized_queue = list_create();
}

t_subscribe_nodo* check_already_subscribed(char *ip, uint32_t puerto,
		t_list *list) {
	int find_subscribed(t_subscribe_nodo *nodo) {
		return (strcmp(ip, nodo->ip) == 0 && (puerto == nodo->puerto));
	}

	return list_find(list, (void*) find_subscribed);
}

void add_to(t_list *list, char *ip, uint32_t puerto, uint32_t fd) {

	t_subscribe_nodo* node = check_already_subscribed(ip, puerto, list);
	if (node == NULL) {
		t_subscribe_nodo *nodo = malloc(sizeof(t_subscribe_nodo));
		nodo->ip = string_duplicate(ip);
		nodo->puerto = puerto;
		nodo->f_desc = fd;
		list_add(list, nodo);
	} else {
		broker_logger_info("Ya esta Subscripto");
		node->f_desc = fd;
	}
}

void search_queue(t_subscribe *subscriber) {

	switch (subscriber->cola) {
	case NEW_QUEUE: {
		broker_logger_info("Subscripto IP: %s, PUERTO: %d,  a Cola NEW ",
				subscriber->ip, subscriber->puerto);
		add_to(new_queue, subscriber->ip, subscriber->puerto,
				subscriber->f_desc);
		break;
	}
	case CATCH_QUEUE: {
		broker_logger_info("Subscripto IP: %s, PUERTO: %d,  a Cola CATCH ",
				subscriber->ip, subscriber->puerto);
		add_to(catch_queue, subscriber->ip, subscriber->puerto,
				subscriber->f_desc);
		break;
	}
	case CAUGHT_QUEUE: {
		broker_logger_info("Subscripto IP: %s, PUERTO: %d,  a Cola CAUGHT ",
				subscriber->ip, subscriber->puerto);
		add_to(caught_queue, subscriber->ip, subscriber->puerto,
				subscriber->f_desc);
		break;
	}
	case GET_QUEUE: {
		broker_logger_info("Subscripto IP: %s, PUERTO: %d,  a Cola GET ",
				subscriber->ip, subscriber->puerto);
		add_to(get_queue, subscriber->ip, subscriber->puerto,
				subscriber->f_desc);
		break;
	}
	case LOCALIZED_QUEUE: {
		broker_logger_info("Subscripto IP: %s, PUERTO: %d,  a Cola LOCALIZED ",
				subscriber->ip, subscriber->puerto);
		add_to(localized_queue, subscriber->ip, subscriber->puerto,
				subscriber->f_desc);
		break;
	}
	case APPEARED_QUEUE: {
		broker_logger_info("Subscripto IP: %s, PUERTO: %d,  a Cola APPEARED ",
				subscriber->ip, subscriber->puerto);
		add_to(appeared_queue, subscriber->ip, subscriber->puerto,
				subscriber->f_desc);
		break;
	}

	}
}

void broker_exit() {
	socket_close_conection(broker_socket);
	broker_config_free();
	broker_logger_destroy();
}

#include "broker.h"

int main(int argc, char *argv[])
{
	if(broker_load() < 0)
		return EXIT_FAILURE;
	broker_server_init();
	broker_exit();

	return EXIT_SUCCESS;
}

int broker_load()
{
	int response = broker_config_load();
	if(response < 0)
		return response;

	response = broker_logger_create(broker_config->log_file);
	if(response < 0)
	{
		broker_config_free();
		return response;
	}
	broker_print_config();

	return 0;
}

void broker_server_init()
{
	broker_socket = socket_create_listener(broker_config->ip_broker, broker_config->puerto_broker);
	if(broker_socket < 0)
	{
		broker_logger_error("Error al crear server");
		return;
	}

	broker_logger_info("Server creado correctamente!! Esperando conexiones...");

	struct sockaddr_in client_info;
	socklen_t addrlen = sizeof client_info;

	pthread_attr_t attrs;
	pthread_attr_init(&attrs);
	pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_JOINABLE);

	for(;;)
	{
		int accepted_fd;
		pthread_t tid;

		if((accepted_fd = accept(broker_socket, (struct sockaddr *) &client_info, &addrlen)) != -1)
		{

			pthread_create(&tid, NULL, (void*) handle_connection, (void*) &accepted_fd);
			pthread_detach(tid);
			broker_logger_info("Creando un hilo para atender una conexión en el socket %d", accepted_fd);
		}
		else
		{
			broker_logger_error("Error al conectar con un cliente");
		}
	}
}
static void *handle_connection(void *arg)
{
	int client_fd = *((int *) arg);

	broker_logger_info("Conexion establecida con cliente: %d", client_fd);
	int received_bytes;
	int protocol;
	while(true)
	{
		received_bytes = recv(client_fd, &protocol, sizeof(int), 0);

		if(received_bytes <= 0)
		{
			broker_logger_error("Error al recibir mensaje");
			return NULL;
		}
		switch(protocol)
		{
			case ACK:{
				broker_logger_info("Recibi un ACK");
				break;
			}

			case NEW_POKEMON:{
				broker_logger_info("Recibi un NEW");
				break;
			}
			case APPEARED_POKEMON:{
				broker_logger_info("Recibi un APPEARED");
				break;
			}


			default:
			break;
		}
	}
}

void broker_exit()
{
	socket_close_conection(broker_socket);
	broker_config_free();
	broker_logger_destroy();
}

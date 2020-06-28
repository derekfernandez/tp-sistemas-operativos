#include "team_planner.h"

bool preemptive;
int fifo_index = 0;
char* split_char = "|";
int deadlocks_detected, deadlocks_resolved = 0, context_switch_qty = 0;

void team_planner_run_planification() {
	sem_wait(&sem_planificador);
	sem_wait(&sem_pokemons_in_ready_queue);
			
	t_entrenador_pokemon* entrenador = team_planner_set_algorithm();

	team_logger_info("Hay un nuevo entrenador en estado EXEC. Id: %d", entrenador->id);
	pthread_mutex_unlock(&entrenador->sem_move_trainers);
	context_switch_qty++;	
}

void team_planner_algoritmo_cercania() {
	team_logger_info("aca empieza");
	sem_wait(&sem_message_on_queue);
	team_logger_info("Se hizo wait a message on queue");
	sem_wait(&sem_entrenadores_disponibles);
	team_logger_info("Se hizo wait a entrenadores disponibles");

	team_logger_info("Se ejecutará el algoritmo de cercanía!");
	
	t_pokemon* pokemon;
	t_entrenador_pokemon* entrenador = malloc(sizeof(t_entrenador_pokemon));


	int c = -1;
	int min_steps = 0;

	t_list*	entrenadores_disponibles = team_planner_create_ready_queue();
	
	for (int i = 0; i < list_size(entrenadores_disponibles); i++) {
		t_entrenador_pokemon* entrenador_aux = list_get(entrenadores_disponibles, i);
		for (int j = 0; j < list_size(pokemon_to_catch); j++) {
			t_pokemon_received* pokemon_con_posiciones_aux = list_get(pokemon_to_catch, j);
			for (int k = 0; k < list_size(pokemon_con_posiciones_aux->pos); k++) {
				t_position* posicion_aux = list_get(pokemon_con_posiciones_aux->pos, k);

				int aux_x = posicion_aux->pos_x - entrenador_aux->position->pos_x;
				int aux_y = posicion_aux->pos_y - entrenador_aux->position->pos_y;

				int closest_sum = fabs(aux_x + aux_y);

				if (c == -1) {
					min_steps = closest_sum;
					c = 0;
				}

				if (closest_sum <= min_steps) {
					pokemon = malloc(sizeof(t_pokemon));
					pokemon->name = string_duplicate(pokemon_con_posiciones_aux->name);
					pokemon->position = malloc(sizeof(t_position));
					pokemon->position->pos_x = posicion_aux->pos_x;
					pokemon->position->pos_y = posicion_aux->pos_y;
					entrenador = entrenador_aux;
				}
			}
		}
	}


	if (team_planner_is_SJF_algorithm()) {
		entrenador->estimated_time = team_planner_calculate_exponential_mean(entrenador->current_burst_time, entrenador->estimated_time);
		team_logger_info("Estimación recalculada del entrenador: %f", entrenador->estimated_time);
		entrenador->current_burst_time = 0;
	}

	if (team_config->algoritmo_planificacion == RR) {
		entrenador->current_burst_time = 0;
	}
	
	entrenador->pokemon_a_atrapar = pokemon;

	add_to_ready_queue(entrenador);
	list_destroy(entrenadores_disponibles);
	team_logger_info("Un entrenador fue agregado a la cola de READY luego de ser seleccionado por el algoritmo de cercanía. id: %d", entrenador->id);
}

void add_to_ready_queue(t_entrenador_pokemon* entrenador) {
	entrenador->state = READY;
	list_add(ready_queue, entrenador);
	delete_from_bloqued_queue(entrenador, 0);
	delete_from_new_queue(entrenador);
	sem_post(&sem_pokemons_in_ready_queue);
}

void delete_from_bloqued_queue(t_entrenador_pokemon* entrenador, int cola) { //0 = READY. 1 = EXIT
	for (int i = 0; i < list_size(block_queue); i++) {
		t_entrenador_pokemon* entrenador_aux = list_get(block_queue, i);
		if (entrenador_aux->id == entrenador->id) {
			list_remove(block_queue, i);
			if (cola == 0) {
				team_logger_info("Se eliminó a un entrenador de la cola BLOCK porque pasará a READY. id: %d", entrenador->id);
			}
			if (cola == 1) {
				team_logger_info("Se eliminó a un entrenador de la cola BLOCK porque pasará a EXIT. id: %d", entrenador->id);
			}
		}
	}
}


void delete_from_new_queue(t_entrenador_pokemon* entrenador) {
	for (int i = 0; i < list_size(new_queue); i++) {
		t_entrenador_pokemon* entrenador_aux = list_get(new_queue, i);
		if (entrenador_aux->id == entrenador->id) {
			list_remove(new_queue, i);
			team_logger_info("Se eliminó al entrenador %d de la cola NEW porque pasará a READY", entrenador->id);
		}
	}
}

t_entrenador_pokemon* team_planner_entrenador_create(int id_entrenador, t_position* posicion, t_list* pokemons, t_list* targets) {
	t_entrenador_pokemon* entrenador = malloc(sizeof(t_entrenador_pokemon));
	entrenador->id = id_entrenador;
	entrenador->state = NEW;
	entrenador->position = malloc(sizeof(t_position));
	entrenador->position = posicion;
	entrenador->pokemons = list_create();
	entrenador->pokemons = pokemons;
	entrenador->targets = list_create();
	entrenador->targets = targets;
	entrenador->wait_time = 0;
	entrenador->current_burst_time = 0;
	entrenador->total_burst_time = 0;
	entrenador->estimated_time = 0;
	entrenador->blocked_info = NULL;
	entrenador->pokemon_a_atrapar = NULL;
	entrenador->deadlock = false;
	pthread_mutex_init(&entrenador->sem_move_trainers, NULL);
	pthread_mutex_lock(&entrenador->sem_move_trainers);
	pthread_create(&entrenador->hilo_entrenador, NULL, (void*) move_trainers_and_catch_pokemon, entrenador);
	pthread_detach(entrenador->hilo_entrenador);

	return entrenador;
}

t_pokemon* team_planner_pokemon_create(char* nombre) {
	t_pokemon* pokemon = malloc(sizeof(t_pokemon));
	pokemon->name = string_duplicate(nombre);
	return pokemon;
}

t_pokemon* team_planner_pokemon_appeared_create(char* nombre, int x, int y) {
	t_pokemon* pokemon = malloc(sizeof(t_pokemon));
	pokemon->name = nombre;
	t_position* pos = malloc(sizeof(t_position));
	pos->pos_x = x;
	pos->pos_y = y;
	pokemon->position = pos;
	return pokemon;
}


char* team_planner_entrenador_string(t_entrenador_pokemon* entrenador) {
	char* entrenador_string = string_new();
	string_append(&entrenador_string, "ID: ");
	string_append(&entrenador_string, string_itoa(entrenador->id));
	string_append(&entrenador_string, " POSICIÓN: (");
	char* posx = string_itoa(entrenador->position->pos_x);
	string_append(&entrenador_string, posx);
	string_append(&entrenador_string, ", ");
	char* posy = string_itoa(entrenador->position->pos_y);
	string_append(&entrenador_string, posy);
	string_append(&entrenador_string, ") POKEMONS: ");
	if(!list_is_empty(entrenador->pokemons)) {
		for (int i = 0; i < list_size(entrenador->pokemons); i++) {
			t_pokemon* pokemon = list_get(entrenador->pokemons, i);
			string_append(&entrenador_string, pokemon->name);
			if (list_get(entrenador->pokemons, i + 1) != NULL) {
				string_append(&entrenador_string, ", ");
			}
		}
	} else {
		string_append(&entrenador_string, "Ninguno");
	}
	string_append(&entrenador_string, " OBJETIVOS: ");
	if(!list_is_empty(entrenador->targets)) {
		for (int i = 0; i < list_size(entrenador->targets); i++) {
			t_pokemon* pokemon = list_get(entrenador->targets, i);
			string_append(&entrenador_string, pokemon->name);
			if (list_get(entrenador->targets, i + 1) != NULL) {
				string_append(&entrenador_string, ", ");
			}
		}
	} else {
		string_append(&entrenador_string, "Ninguno");
	}

	return entrenador_string;
}

t_position* team_planner_extract_position(char* pos_spl) {
	char** splitted = string_split(pos_spl, split_char);
	t_position* posicion = malloc(sizeof(t_position));
	posicion->pos_x = atoi(utils_get_parameter_i(splitted, 0));
	posicion->pos_y = atoi(utils_get_parameter_i(splitted, 1));
	utils_free_array(splitted);

	return posicion;
}

void team_planner_extract_pokemons(t_list* pokemons, char* pokes_spl) {
	char** splitted = string_split(pokes_spl, split_char);
	int pokes_spl_size = utils_get_array_size(splitted);
	for (int j = 0; j < pokes_spl_size; j++) {
		char* nombre = string_duplicate(utils_get_parameter_i(splitted, j));
		t_pokemon* pokemon = team_planner_pokemon_create(nombre);
		list_add(pokemons, pokemon);
	}
	utils_free_array(splitted);
}

void planner_init_global_targets(t_list* objetivos) {
	for (int i = 0; i < list_size(objetivos); i++) {
		t_pokemon* pokemon = list_get(objetivos, i);
		char* pokemon_name = string_duplicate(pokemon->name);
		int cantidad_pokemon = (int) dictionary_get(team_planner_global_targets, pokemon_name);
		if (cantidad_pokemon == 0) {
			dictionary_put(team_planner_global_targets, pokemon_name, (void *) 1);
			list_add(keys_list, pokemon_name);
		} else {
			cantidad_pokemon++;
			dictionary_put(team_planner_global_targets, pokemon_name, (void *) cantidad_pokemon);
		}
	}
	sem_post(&sem_pokemons_to_get);
}

char* planner_print_global_targets() {
	char* global_targets_string = string_new();
	string_append(&global_targets_string, "POKEMON			|CANTIDAD		\n");
	for (int i = 0; i < list_size(keys_list); i++) {
		char* nombre_pokemon = list_get(keys_list, i);
		int j = 10 - strlen(nombre_pokemon);
		int cantidad_pokemon = (int) dictionary_get(team_planner_global_targets, nombre_pokemon);
		char* target_string = string_new();
		for (int k = 0; k < j; k++) {
			strcat(nombre_pokemon, " ");
		}
		string_append(&target_string, nombre_pokemon);
		string_append(&target_string, "		| ");
		string_append(&target_string, string_itoa(cantidad_pokemon));
		string_append(&target_string, "		\n");
		string_append(&global_targets_string, target_string);
	}
	return global_targets_string;
}

void team_planner_add_new_trainner(t_entrenador_pokemon* entrenador) {
	list_add(new_queue, entrenador);
	team_logger_info("Se añadió a un entrenador %d a la cola NEW", entrenador->id);
	sem_post(&sem_entrenadores_disponibles);
}

void team_planner_finish_trainner(t_entrenador_pokemon* entrenador) {
	entrenador->state = EXIT;
	entrenador->blocked_info->blocked_time = 0;
	entrenador->blocked_info->status = 0;
	entrenador->pokemon_a_atrapar = NULL;
	entrenador->deadlock = false;
	delete_from_bloqued_queue(entrenador, 1);
	list_add(exit_queue, entrenador);
}

void team_planner_change_block_status_by_id_corr(int status, uint32_t id_corr) {
	t_entrenador_info_bloqueo* info_bloqueo = malloc(sizeof(t_entrenador_info_bloqueo)); 
	info_bloqueo->blocked_time = 0;
	info_bloqueo->status = status;
	
	
	t_entrenador_pokemon* entrenador = find_trainer_by_id_corr(id_corr);
	entrenador->state = BLOCK;
	entrenador->pokemon_a_atrapar = NULL;

	entrenador->blocked_info = info_bloqueo;

	if (status == 0 && entrenador->deadlock == false) {
		sem_post(&sem_entrenadores_disponibles); 
	}
}

void team_planner_change_block_status_by_trainer(int status, t_entrenador_pokemon* entrenador) {
	t_entrenador_info_bloqueo* info_bloqueo = malloc(sizeof(t_entrenador_info_bloqueo)); 
	info_bloqueo->blocked_time = 0;
	info_bloqueo->status = status;
	entrenador->state = BLOCK;
	entrenador->pokemon_a_atrapar = NULL;
	
	entrenador->blocked_info = info_bloqueo;

	if (status == 0 && entrenador->deadlock == false) {		
		sem_post(&sem_entrenadores_disponibles);
	}
}

t_entrenador_pokemon* find_trainer_by_id_corr(uint32_t id) { 
	int _id_match(t_entrenador_pokemon *entrenador) {
		for (int i = 0; i < list_size(entrenador->list_id_catch); i++) {
			uint32_t id_corr = (uint32_t) list_get(entrenador->list_id_catch, i);
			if (id_corr == id) {
				return 1;
			}
		}
		return 0;
	}
	return list_find(block_queue, (void*) _id_match);
}

void planner_load_entrenadores() {
	int i = 0;

	keys_list = list_create();
	target_pokemons = list_create();
	team_planner_global_targets = dictionary_create();
	while (team_config->posiciones_entrenadores[i] != NULL) {
		t_position* posicion = team_planner_extract_position(team_config->posiciones_entrenadores[i]);
		t_list* pokemons = list_create();
		if(team_config->posiciones_entrenadores[i] != NULL && team_config->pokemon_entrenadores[i] != NULL) {
			team_planner_extract_pokemons(pokemons, team_config->pokemon_entrenadores[i]);
		}
		t_list* objetivos = list_create();
		if(team_config->posiciones_entrenadores[i] != NULL && team_config->objetivos_entrenadores[i] != NULL) {
			team_planner_extract_pokemons(objetivos, team_config->objetivos_entrenadores[i]);
		}

		t_entrenador_pokemon* entrenador = team_planner_entrenador_create(i, posicion, pokemons, objetivos);
		team_logger_info("Se creó el entrenador: %s", team_planner_entrenador_string(entrenador));
		team_planner_add_new_trainner(entrenador);
		list_add_all(target_pokemons, objetivos);
		planner_init_global_targets(objetivos);
		i++;
	}
	sem_post(&sem_entrenadores_disponibles);

	team_logger_info("Hay %d entrenadores en la cola de NEW", list_size(new_queue));
	int tamanio_objetivos = 0;
	void add_total_targets(char* ___, int pokemons_qty) {
		tamanio_objetivos +=pokemons_qty;
	}

	dictionary_iterator(team_planner_global_targets, (void*) add_total_targets);
	char* objetivos_to_string = planner_print_global_targets();
	team_logger_info("Hay %d objetivos globales: \n%s", tamanio_objetivos, objetivos_to_string);
	free(objetivos_to_string);
}

void planner_init_quees() {
	new_queue = list_create();
	ready_queue = list_create();
	block_queue = list_create();
	exit_queue = list_create();
	pokemon_to_catch = list_create();
}

int team_planner_get_least_estimate_index() {
	int least_index = 0;
	float lower_estimate = 100000.0;
	for (int i = 0; i < list_size(ready_queue); i++) {
		t_entrenador_pokemon* entrenador = list_get(ready_queue, i);
		if (entrenador == NULL)
			continue;
		if (entrenador->estimated_time < lower_estimate) {
			lower_estimate = entrenador->estimated_time;
			least_index = i; //para SJF
		}
	}

	return least_index;
}

void new_cpu_cicle() {
	int i = 0;

	for (i = 0; i < list_size(ready_queue); i++) {
		t_entrenador_pokemon* trainner = list_get(ready_queue, i);
		trainner->wait_time++;
	}

	for (i = 0; i < list_size(block_queue); i++) {
		t_entrenador_pokemon* trainner = list_get(block_queue, i);
		trainner->blocked_info->blocked_time++;
	}
}

float team_planner_calculate_exponential_mean(int burst_time, float tn) {
	float alpha = 0.5f;
	//tn+1 = α*tn + (1 - α)*tn
	float next_tn = alpha * (float) burst_time + (1.0 - alpha) * tn;
	return next_tn;
}

t_entrenador_pokemon* team_planner_exec_trainer(t_entrenador_pokemon* entrenador) {
	entrenador->wait_time = 0;
	entrenador->current_burst_time = 0;
	entrenador->state = EXEC;

	return entrenador;
}

bool team_planner_is_SJF_algorithm() {
	return team_config->algoritmo_planificacion == SJF_CD || team_config->algoritmo_planificacion == SJF_SD;
}

t_list* filter_block_list_by_0() {
	bool _is_available(t_entrenador_pokemon* trainner) {
		return trainner->blocked_info->status == 0 && trainner->deadlock == false;
	}
	t_list* blocked_but_to_exec = list_filter(block_queue, (void*) _is_available);
	return blocked_but_to_exec;
}
//que hace acá. repite código
t_list* team_planner_create_ready_queue() {	
	bool _is_available(t_entrenador_pokemon* trainner) {
		return trainner->blocked_info->status == 0 && trainner->deadlock == false;
	}
	t_list* bloquados_en_cero = filter_block_list_by_0(block_queue, (void*) _is_available);
	
	t_list* listo_para_planificar = list_create();
	list_add_all(listo_para_planificar, bloquados_en_cero);
	list_add_all(listo_para_planificar, new_queue);

	return listo_para_planificar;
}
//SJF
t_entrenador_pokemon* team_planner_apply_SJF() {
	
	int least_estimate_index = team_planner_get_least_estimate_index();	
	t_entrenador_pokemon* entrenador = list_get(ready_queue, least_estimate_index);
	list_remove(ready_queue, least_estimate_index);	
	team_logger_info("Se eliminó al entrenador %d de la cola de READY porque es su turno de ejecutar", entrenador->id);
	
	return team_planner_exec_trainer(entrenador);	
}
//FIFO
t_entrenador_pokemon* team_planner_apply_FIFO() {
	
	int next_out_index = fifo_index;
	t_entrenador_pokemon* entrenador;
	if (next_out_index <= list_size(ready_queue)) { 
		entrenador = list_get(ready_queue, next_out_index);
		list_remove(ready_queue, next_out_index);
		team_logger_info("Se eliminó al entrenador %d de la cola de READY porque es su turno de ejecutar", entrenador->id);
		fifo_index++;
	}	
	return team_planner_exec_trainer(entrenador);
}
//RR
t_entrenador_pokemon* team_planner_apply_RR() {
	
	t_entrenador_pokemon* entrenador = NULL;
	int next_out_index = fifo_index;
	if (next_out_index < list_size(ready_queue)) {
		t_entrenador_pokemon* entrenador = list_get(ready_queue, next_out_index);
		list_remove(ready_queue, next_out_index);
		fifo_index++;
		team_logger_info("Se eliminó al entrenador %d de la cola de READY porque es su turno de ejecutar", entrenador->id);
	}
	return team_planner_exec_trainer(entrenador);	
}

t_entrenador_pokemon* team_planner_set_algorithm() {
	switch (team_config->algoritmo_planificacion) {
		case FIFO:
			return team_planner_apply_FIFO();
			break;
		case RR:
			return team_planner_apply_RR();
			break;
		case SJF_CD:
			return team_planner_apply_SJF();
			break;
		case SJF_SD:
			return team_planner_apply_SJF();
			break;
	}
	return NULL;
}
//se llamará cuando quiera verificar que lo unico que puedo hacer a partir de ahora es solucionar deadlock
bool all_queues_are_empty_except_block() {
	t_list* entrenadores_disponibles = team_planner_create_ready_queue();
	return list_is_empty(entrenadores_disponibles) && list_is_empty(ready_queue);
}
//DEADLOCK
void solve_deadlock() {

	sem_init(&sem_deadlock, 0, 0);

	team_logger_info("Se iniciará la detección y resolución de deadlocks!");
	int i = 0;

	while (block_queue_is_not_empty()) {

		t_entrenador_pokemon* entrenador_bloqueante = list_get(block_queue, i);
		char* pokemon_de_entrenador_bloqueante = ver_a_quien_no_necesita(entrenador_bloqueante);

		t_entrenador_pokemon* entrenador_bloqueado = entrenador_que_necesita(pokemon_de_entrenador_bloqueante);

		entrenador_bloqueado->pokemon_a_atrapar->name = pokemon_de_entrenador_bloqueante;
		entrenador_bloqueado->pokemon_a_atrapar->position->pos_x = entrenador_bloqueante->position->pos_x;
		entrenador_bloqueado->pokemon_a_atrapar->position->pos_y = entrenador_bloqueante->position->pos_y;

		deadlocks_detected++;	

		team_logger_info("Se detectó un deadlock. El entrenador %d", entrenador_bloqueante->id, " está bloqueando al entrenador %d", entrenador_bloqueado->id);

		add_to_ready_queue(entrenador_bloqueado);

		sem_wait(&sem_deadlock);
		char* pokemon_de_entrenador_bloqueado = ver_a_quien_no_necesita(entrenador_bloqueado);

		list_add(entrenador_bloqueado->pokemons, pokemon_de_entrenador_bloqueante);
		list_add(entrenador_bloqueante->pokemons, pokemon_de_entrenador_bloqueado);
		remove_from_pokemons_list(entrenador_bloqueado, pokemon_de_entrenador_bloqueado);
		remove_from_pokemons_list(entrenador_bloqueante, pokemon_de_entrenador_bloqueante);	

		if (trainer_completed_with_success(entrenador_bloqueado)) {
			team_planner_finish_trainner(entrenador_bloqueado);
		}

		if (trainer_completed_with_success(entrenador_bloqueante)) {
			team_planner_finish_trainner(entrenador_bloqueante);
		}
		i++;
		deadlocks_resolved++;	
	}
	team_logger_info("Finaliza el algoritmo de detección de interbloqueos!");
}

void remove_from_pokemons_list(t_entrenador_pokemon* entrenador, char* pokemon) {
	for (int i = 0; i < list_size(entrenador->pokemons); i++) {
		char* pokemon_aux = list_get(entrenador->pokemons, i);

		if (string_equals_ignore_case(pokemon, pokemon_aux)) {
			list_remove(entrenador->pokemons, i);
			break;
		}
	}
}

bool block_queue_is_not_empty() {
	return !list_is_empty(block_queue);
}

t_entrenador_pokemon* entrenador_que_necesita(char* pokemon_de_entrenador_bloqueado) {
	int i = 0;
	bool lo_tiene = true;

	while (i < list_size(block_queue)) {
		t_entrenador_pokemon* entrenador = list_get(block_queue, i);
		
		for (int j = 0; j < list_size(entrenador->targets); j++) {
			char* pokemon_objetivo = list_get(entrenador->targets, j);
			if (string_equals_ignore_case(pokemon_objetivo, pokemon_de_entrenador_bloqueado)) {
				lo_tiene = true; 
				break;
			} else {
				lo_tiene = false;
			} 
		}

		if (lo_tiene) {
			i++; 
		} else {
			i = list_size(block_queue); //corta el while
			return entrenador;
		}
	}
	return NULL;
}

char* ver_a_quien_no_necesita(t_entrenador_pokemon* entrenador) {
	int i = 0;
	bool le_sirve = true;

	while (i < list_size(entrenador->pokemons)) {
		char* pokemon_a_entregar = list_get(entrenador->pokemons, i); //agarro al primero de la lista de los que tengo
		
		for (int j = 0; j < list_size(entrenador->targets); j++) {	 //recorro la lista de objetivos
			char* pokemon_objetivo = list_get(entrenador->targets, j);
			if (string_equals_ignore_case(pokemon_objetivo, pokemon_a_entregar)) {
				le_sirve = true; //si son iguales agarro al segundo de la lista de los que tengo para volver a comparar
				break;
			} else {
				le_sirve = false;
			} //si no le sirve y todavía no termino de recorrer los obtenidos sigue. si ya termino entonces controla
		}

		if (le_sirve) {
			i++; //si finalmente le sirve busca al prox
		} else {
			i = list_size(entrenador->pokemons); //corta el while
			return pokemon_a_entregar;
		}
	}
	return NULL;
}

t_list* team_planner_get_trainners() {
	t_list* trainners = list_create();
	//list_add(trainners, exec_entrenador); //TODO: si quiero todos y el exec no existe siempre va a faltar 1
	list_add_all(trainners, new_queue);
	list_add_all(trainners, ready_queue);
	list_add_all(trainners, block_queue);
	list_add_all(trainners, exit_queue);

	return trainners;
}

void team_planner_print_fullfill_target() {
	t_list* trainners = team_planner_get_trainners();

	int add_burst_time(int accum, t_entrenador_pokemon* trainner) {
      return accum + trainner->current_burst_time;
	}
	int total_cpu = (int) list_fold(trainners, 0, (void*) add_burst_time);
	team_logger_info("Cantidad de ciclos de CPU totales: %d", total_cpu);
	team_logger_info("Cantidad de cambios de contexto realizados: %d", context_switch_qty);
	team_logger_info("Cantidad de ciclos de CPU realizados por entrenador:");

	void _list_burst_trainner(t_entrenador_pokemon *trainner) {
		team_logger_info("Entrenador %d -> Ciclos de CPU realizados: %d", trainner->id, trainner->total_burst_time);
	}
	list_iterate(trainners, (void*) _list_burst_trainner);

	list_destroy(trainners);
	team_logger_info("Deadlocks producidos: %d  y resueltos: %d", deadlocks_detected, deadlocks_resolved);
}

void team_planner_init() {
	team_logger_info("Planificador de TEAM iniciando estructuras!");
	planner_init_quees();
	planner_load_entrenadores();	
}

bool trainer_completed_with_success(t_entrenador_pokemon* entrenador) {

	if (list_size(entrenador->pokemons) == list_size(entrenador->targets)) {
		t_list* pokemons_target_aux = list_create();
		pokemons_target_aux = entrenador->targets;

		for (int i = 0; i<list_size(entrenador->pokemons); i++) {
			t_pokemon* pokemon = list_get(entrenador->pokemons, i);

			for (int j = 0; j<list_size(pokemons_target_aux); j++) {
				t_pokemon* pokemon_aux = list_get(pokemons_target_aux, j);

				if (string_equals_ignore_case(pokemon->name, pokemon_aux->name)) {
					list_remove(pokemons_target_aux, j);
				}
			}
		}

		if (list_size(pokemons_target_aux) == 0) {
			return true;
		}
	}
	return false;
}


void planner_destroy_pokemons(t_pokemon* pokemon) {
	free(pokemon->name);
	free(pokemon);
}

void planner_destroy_entrenador(t_entrenador_pokemon* entrenador) {
	free(entrenador->position);
	list_destroy_and_destroy_elements(entrenador->pokemons, (void*)planner_destroy_pokemons);
	free(entrenador->pokemons);
	list_destroy_and_destroy_elements(entrenador->targets, (void*)planner_destroy_pokemons);
	free(entrenador->targets);
	free(entrenador);
}

void planner_destroy_global_targets(t_dictionary* global_targets) {
	dictionary_destroy_and_destroy_elements(global_targets, (void*)planner_destroy_pokemons);
}

void planner_destroy_quees() {
	//planner_destroy_entrenador(exec_entrenador);
	list_destroy_and_destroy_elements(new_queue, (void*)planner_destroy_entrenador);
	list_destroy_and_destroy_elements(ready_queue, (void*)planner_destroy_entrenador);
	list_destroy_and_destroy_elements(block_queue, (void*)planner_destroy_entrenador);
	list_destroy_and_destroy_elements(exit_queue, (void*)planner_destroy_entrenador);	
	list_destroy_and_destroy_elements(pokemon_to_catch, (void*)planner_destroy_entrenador);
	list_destroy(keys_list);
	list_destroy(target_pokemons);
}

void team_planner_destroy() {
	sem_destroy(&sem_entrenadores_disponibles);
	sem_destroy(&sem_message_on_queue);
	sem_destroy(&sem_pokemons_in_ready_queue);
	planner_destroy_quees();
	planner_destroy_global_targets(team_planner_global_targets);
}

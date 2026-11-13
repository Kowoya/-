/*
 * Система для ефективного виконання серії завдань.
 *
 *   Користувач --(серія завдань)--> [Інтерфейсна частина (UI)]
 *                                         |  pipe: параметри  (UI -> Executive)
 *                                         v
 *                                   [Виконавча частина (Executive)]
 *                                         |  pipe: набір результатів (Executive -> UI)
 *                                         v
 *   Користувач <--(результат кожного завдання)-- UI консолідує результати
 *
 * Обидві частини - окремі ОДНОПОТОКОВІ процеси (fork), зв'язані двома pipe:
 *   - request_pipe : UI -> Executive (тільки параметри, в один бік)
 *   - response_pipe: Executive -> UI (тільки результати, у відповідь)
 *
 * Завдання = вхідне значення + перелік обробників.
 * Словник доступних обробників зберігається у виконавчій частині.
 * Результати обробників одного завдання консолідує UI вибраною функцією
 * (за замовчуванням - добуток). Перед виходом UI виводить зведену статистику.
 *
 * Збирання:  gcc -std=c11 -Wall -Wextra -O2 task_system.c -o task_system -lm
 * Запуск:
 *   ./task_system                              - інтерактивно (система запитує параметри)
 *   ./task_system 5:sq,inc 3:fact,dbl          - завдання з командного рядка
 *   ./task_system -c sum 5:sq,inc              - інша функція консолідації
 *   Функції консолідації: prod, sum, min, max, avg
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ======================= Протокол обміну ======================= */

#define MAX_HANDLERS_PER_TASK 16
#define HANDLER_NAME_LEN 16
#define HANDLER_DESCR_LEN 48
#define MAX_DICTIONARY_SIZE 32

enum request_type {
    REQ_TASK = 1, /* виконати завдання */
    REQ_LIST = 2, /* надіслати словник обробників */
    REQ_QUIT = 3  /* завершити роботу */
};

enum result_status {
    ST_OK = 0,
    ST_UNKNOWN_HANDLER = 1, /* обробника немає в словнику */
    ST_DOMAIN_ERROR = 2,    /* значення поза областю визначення */
    ST_OVERFLOW = 3,        /* результат не скінченний */
    ST_STATUS_COUNT
};

static const char *status_text[ST_STATUS_COUNT] = {
    "ok", "невідомий обробник", "поза областю визначення", "переповнення"
};

struct task_request {
    int type;
    int task_id;
    double value;
    int handler_count;
    char handler_names[MAX_HANDLERS_PER_TASK][HANDLER_NAME_LEN];
};

struct handler_result {
    int status;
    double value;
};

struct task_response {
    int task_id;
    int handler_count;
    struct handler_result results[MAX_HANDLERS_PER_TASK];
};

struct dictionary_response {
    int count;
    char names[MAX_DICTIONARY_SIZE][HANDLER_NAME_LEN];
    char descriptions[MAX_DICTIONARY_SIZE][HANDLER_DESCR_LEN];
};

/* Надійні читання/запис: pipe може передати дані частинами */
static int write_all(int fd, const void *buffer, size_t size)
{
    const char *ptr = buffer;
    while (size > 0) {
        ssize_t written = write(fd, ptr, size);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        ptr += written;
        size -= (size_t)written;
    }
    return 0;
}

/* Повертає 1 - прочитано повністю, 0 - кінець потоку, -1 - помилка */
static int read_all(int fd, void *buffer, size_t size)
{
    char *ptr = buffer;
    size_t total = 0;
    while (total < size) {
        ssize_t got = read(fd, ptr + total, size - total);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) return total == 0 ? 0 : -1;
        total += (size_t)got;
    }
    return 1;
}

/* ======================= Виконавча частина ======================= */

typedef int (*handler_fn)(double input, double *output);

static int h_square(double x, double *out) { *out = x * x; return ST_OK; }
static int h_cube(double x, double *out)   { *out = x * x * x; return ST_OK; }
static int h_inc(double x, double *out)    { *out = x + 1; return ST_OK; }
static int h_dec(double x, double *out)    { *out = x - 1; return ST_OK; }
static int h_double(double x, double *out) { *out = x * 2; return ST_OK; }
static int h_half(double x, double *out)   { *out = x / 2; return ST_OK; }
static int h_neg(double x, double *out)    { *out = -x; return ST_OK; }
static int h_abs(double x, double *out)    { *out = fabs(x); return ST_OK; }

static int h_sqrt(double x, double *out)
{
    if (x < 0) return ST_DOMAIN_ERROR;
    *out = sqrt(x);
    return ST_OK;
}

static int h_inverse(double x, double *out)
{
    if (x == 0) return ST_DOMAIN_ERROR;
    *out = 1.0 / x;
    return ST_OK;
}

static int h_ln(double x, double *out)
{
    if (x <= 0) return ST_DOMAIN_ERROR;
    *out = log(x);
    return ST_OK;
}

static int h_factorial(double x, double *out)
{
    if (x < 0 || x != floor(x)) return ST_DOMAIN_ERROR;
    if (x > 170) return ST_OVERFLOW;
    double result = 1;
    for (int i = 2; i <= (int)x; i++) result *= i;
    *out = result;
    return ST_OK;
}

struct handler_entry {
    const char *name;
    const char *description;
    handler_fn function;
};

/* Словник доступних обробників */
static const struct handler_entry handler_dictionary[] = {
    { "sq",   "x^2",                h_square },
    { "cube", "x^3",                h_cube },
    { "inc",  "x + 1",              h_inc },
    { "dec",  "x - 1",              h_dec },
    { "dbl",  "2x",                 h_double },
    { "half", "x / 2",              h_half },
    { "neg",  "-x",                 h_neg },
    { "abs",  "|x|",                h_abs },
    { "sqrt", "корінь x (x >= 0)",  h_sqrt },
    { "inv",  "1 / x (x != 0)",     h_inverse },
    { "ln",   "ln x (x > 0)",       h_ln },
    { "fact", "x! (ціле 0..170)",   h_factorial },
};
static const int handler_dictionary_size =
    (int)(sizeof handler_dictionary / sizeof handler_dictionary[0]);

static const struct handler_entry *find_handler(const char *name)
{
    for (int i = 0; i < handler_dictionary_size; i++)
        if (strcmp(handler_dictionary[i].name, name) == 0)
            return &handler_dictionary[i];
    return NULL;
}

static void execute_task(const struct task_request *request,
                         struct task_response *response)
{
    memset(response, 0, sizeof *response);
    response->task_id = request->task_id;
    response->handler_count = request->handler_count;

    /* Значення подається на вхід кожному обробнику завдання */
    for (int i = 0; i < request->handler_count; i++) {
        struct handler_result *result = &response->results[i];
        const struct handler_entry *handler = find_handler(request->handler_names[i]);
        if (handler == NULL) {
            result->status = ST_UNKNOWN_HANDLER;
            continue;
        }
        double output = 0;
        result->status = handler->function(request->value, &output);
        if (result->status == ST_OK && !isfinite(output))
            result->status = ST_OVERFLOW;
        result->value = output;
    }
}

static void executive_main(int request_fd, int response_fd)
{
    struct task_request request;
    for (;;) {
        int read_status = read_all(request_fd, &request, sizeof request);
        if (read_status <= 0) break; /* UI закрив канал або помилка */

        if (request.type == REQ_QUIT) break;

        if (request.type == REQ_LIST) {
            struct dictionary_response dictionary;
            memset(&dictionary, 0, sizeof dictionary);
            dictionary.count = handler_dictionary_size;
            for (int i = 0; i < handler_dictionary_size; i++) {
                snprintf(dictionary.names[i], HANDLER_NAME_LEN, "%s",
                         handler_dictionary[i].name);
                snprintf(dictionary.descriptions[i], HANDLER_DESCR_LEN, "%s",
                         handler_dictionary[i].description);
            }
            if (write_all(response_fd, &dictionary, sizeof dictionary) < 0) break;
            continue;
        }

        if (request.type == REQ_TASK) {
            if (request.handler_count < 0 || request.handler_count > MAX_HANDLERS_PER_TASK)
                request.handler_count = 0;
            struct task_response response;
            execute_task(&request, &response);
            if (write_all(response_fd, &response, sizeof response) < 0) break;
        }
    }
    close(request_fd);
    close(response_fd);
}

/* ======================= Інтерфейсна частина ======================= */

typedef double (*consolidate_fn)(const double *values, int count);

static double consolidate_product(const double *v, int n)
{
    double r = 1;
    for (int i = 0; i < n; i++) r *= v[i];
    return r;
}

static double consolidate_sum(const double *v, int n)
{
    double r = 0;
    for (int i = 0; i < n; i++) r += v[i];
    return r;
}

static double consolidate_min(const double *v, int n)
{
    double r = v[0];
    for (int i = 1; i < n; i++) if (v[i] < r) r = v[i];
    return r;
}

static double consolidate_max(const double *v, int n)
{
    double r = v[0];
    for (int i = 1; i < n; i++) if (v[i] > r) r = v[i];
    return r;
}

static double consolidate_average(const double *v, int n)
{
    return consolidate_sum(v, n) / n;
}

struct consolidator_entry {
    const char *name;
    consolidate_fn function;
};

static const struct consolidator_entry consolidators[] = {
    { "prod", consolidate_product },
    { "sum",  consolidate_sum },
    { "min",  consolidate_min },
    { "max",  consolidate_max },
    { "avg",  consolidate_average },
};

struct handler_usage {
    char name[HANDLER_NAME_LEN];
    int calls;
    int errors;
};

struct session_statistics {
    int tasks_total;
    int tasks_ok;
    int tasks_failed;
    int handler_calls;
    int status_counts[ST_STATUS_COUNT];
    double consolidated_min;
    double consolidated_max;
    double consolidated_sum;
    struct handler_usage usage[MAX_DICTIONARY_SIZE + MAX_HANDLERS_PER_TASK];
    int usage_count;
    struct timespec start_time;
};

struct ui_context {
    int request_fd;
    int response_fd;
    const struct consolidator_entry *consolidator;
    struct dictionary_response dictionary;
    struct session_statistics stats;
};

static void record_handler_usage(struct session_statistics *stats,
                                 const char *name, int status)
{
    int index = -1;
    for (int i = 0; i < stats->usage_count; i++)
        if (strcmp(stats->usage[i].name, name) == 0) { index = i; break; }
    if (index < 0) {
        int capacity = (int)(sizeof stats->usage / sizeof stats->usage[0]);
        if (stats->usage_count >= capacity) return;
        index = stats->usage_count++;
        snprintf(stats->usage[index].name, HANDLER_NAME_LEN, "%s", name);
    }
    stats->usage[index].calls++;
    if (status != ST_OK) stats->usage[index].errors++;
}

static int request_dictionary(struct ui_context *ui)
{
    struct task_request request;
    memset(&request, 0, sizeof request);
    request.type = REQ_LIST;
    if (write_all(ui->request_fd, &request, sizeof request) < 0) return -1;
    return read_all(ui->response_fd, &ui->dictionary, sizeof ui->dictionary) == 1 ? 0 : -1;
}

static void print_dictionary(const struct ui_context *ui)
{
    printf("Доступні обробники:\n");
    for (int i = 0; i < ui->dictionary.count; i++)
        printf("  %-6s %s\n", ui->dictionary.names[i], ui->dictionary.descriptions[i]);
}

/* Розбирає список обробників, розділених пробілами або комами */
static int parse_handler_list(char *text, struct task_request *request)
{
    request->handler_count = 0;
    for (char *token = strtok(text, " ,\t\n"); token != NULL;
         token = strtok(NULL, " ,\t\n")) {
        if (request->handler_count >= MAX_HANDLERS_PER_TASK) {
            fprintf(stderr, "Забагато обробників (максимум %d)\n", MAX_HANDLERS_PER_TASK);
            return -1;
        }
        snprintf(request->handler_names[request->handler_count++],
                 HANDLER_NAME_LEN, "%s", token);
    }
    if (request->handler_count == 0) {
        fprintf(stderr, "Не вказано жодного обробника\n");
        return -1;
    }
    return 0;
}

static int parse_value(const char *text, double *value)
{
    char *end = NULL;
    errno = 0;
    *value = strtod(text, &end);
    if (end == text || errno != 0) return -1;
    while (isspace((unsigned char)*end)) end++;
    return *end == '\0' ? 0 : -1;
}

/* Надсилає завдання, отримує результати, консолідує і виводить.
   Повертає -1, якщо зв'язок з виконавчою частиною втрачено. */
static int run_task(struct ui_context *ui, struct task_request *request)
{
    struct session_statistics *stats = &ui->stats;
    request->type = REQ_TASK;
    request->task_id = ++stats->tasks_total;

    if (write_all(ui->request_fd, request, sizeof *request) < 0) return -1;

    struct task_response response;
    if (read_all(ui->response_fd, &response, sizeof response) != 1) return -1;

    printf("Завдання #%d: x = %g\n", response.task_id, request->value);

    double ok_values[MAX_HANDLERS_PER_TASK];
    int ok_count = 0;
    for (int i = 0; i < response.handler_count; i++) {
        const struct handler_result *result = &response.results[i];
        int status = (result->status >= 0 && result->status < ST_STATUS_COUNT)
                         ? result->status : ST_UNKNOWN_HANDLER;
        stats->handler_calls++;
        stats->status_counts[status]++;
        record_handler_usage(stats, request->handler_names[i], status);

        if (status == ST_OK) {
            ok_values[ok_count++] = result->value;
            printf("  %-6s -> %g\n", request->handler_names[i], result->value);
        } else {
            printf("  %-6s -> помилка: %s\n", request->handler_names[i], status_text[status]);
        }
    }

    if (ok_count == 0) {
        stats->tasks_failed++;
        printf("  Результат: немає успішних обробників\n\n");
        return 0;
    }

    double consolidated = ui->consolidator->function(ok_values, ok_count);
    if (stats->tasks_ok == 0 || consolidated < stats->consolidated_min)
        stats->consolidated_min = consolidated;
    if (stats->tasks_ok == 0 || consolidated > stats->consolidated_max)
        stats->consolidated_max = consolidated;
    stats->consolidated_sum += consolidated;
    stats->tasks_ok++;

    printf("  Результат (%s): %g\n\n", ui->consolidator->name, consolidated);
    return 0;
}

static void print_statistics(const struct ui_context *ui)
{
    const struct session_statistics *stats = &ui->stats;
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed_ms = (end_time.tv_sec - stats->start_time.tv_sec) * 1e3 +
                        (end_time.tv_nsec - stats->start_time.tv_nsec) / 1e6;

    printf("=========== Зведена статистика ===========\n");
    printf("Функція консолідації:      %s\n", ui->consolidator->name);
    printf("Завдань виконано:          %d\n", stats->tasks_total);
    printf("  успішних:                %d\n", stats->tasks_ok);
    printf("  без результату:          %d\n", stats->tasks_failed);
    printf("Викликів обробників:       %d\n", stats->handler_calls);
    for (int s = 0; s < ST_STATUS_COUNT; s++)
        if (stats->status_counts[s] > 0)
            printf("  %s: %d\n", status_text[s], stats->status_counts[s]);
    if (stats->tasks_ok > 0) {
        printf("Результати завдань: min = %g, max = %g, середнє = %g\n",
               stats->consolidated_min, stats->consolidated_max,
               stats->consolidated_sum / stats->tasks_ok);
    }
    if (stats->usage_count > 0) {
        printf("Використання обробників:\n");
        for (int i = 0; i < stats->usage_count; i++)
            printf("  %-6s викликів: %d, помилок: %d\n", stats->usage[i].name,
                   stats->usage[i].calls, stats->usage[i].errors);
    }
    printf("Час роботи:                %.3f мс\n", elapsed_ms);
    printf("==========================================\n");
}

/* Завдання з командного рядка у форматі  значення:обробник1,обробник2 */
static int run_argument_tasks(struct ui_context *ui, char **tasks, int task_count)
{
    for (int i = 0; i < task_count; i++) {
        char buffer[512];
        snprintf(buffer, sizeof buffer, "%s", tasks[i]);
        char *separator = strchr(buffer, ':');
        if (separator == NULL) {
            fprintf(stderr, "Пропущено \"%s\": очікується формат значення:обробники\n", tasks[i]);
            continue;
        }
        *separator = '\0';

        struct task_request request;
        memset(&request, 0, sizeof request);
        if (parse_value(buffer, &request.value) < 0) {
            fprintf(stderr, "Пропущено \"%s\": некоректне значення\n", tasks[i]);
            continue;
        }
        if (parse_handler_list(separator + 1, &request) < 0) continue;
        if (run_task(ui, &request) < 0) return -1;
    }
    return 0;
}

/* Система запитує параметри кожного завдання */
static int run_interactive(struct ui_context *ui)
{
    char line[512];
    print_dictionary(ui);
    printf("Команди: list - словник обробників, q - вихід\n\n");

    for (;;) {
        printf("Вхідне значення: ");
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL) break;
        line[strcspn(line, "\n")] = '\0';
        if (strcmp(line, "q") == 0) break;
        if (strcmp(line, "list") == 0) { print_dictionary(ui); continue; }
        if (line[0] == '\0') continue;

        struct task_request request;
        memset(&request, 0, sizeof request);
        if (parse_value(line, &request.value) < 0) {
            printf("Некоректне число\n");
            continue;
        }

        printf("Обробники (через пробіл або кому): ");
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL) break;
        if (parse_handler_list(line, &request) < 0) continue;

        if (run_task(ui, &request) < 0) return -1;
    }
    return 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Використання: %s [-c prod|sum|min|max|avg] [значення:обробник,обробник ...]\n"
            "Без завдань у командному рядку система запитує параметри інтерактивно.\n",
            program);
}

int main(int argc, char **argv)
{
    const struct consolidator_entry *consolidator = &consolidators[0];
    int first_task_arg = 1;

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        consolidator = NULL;
        int consolidator_count = (int)(sizeof consolidators / sizeof consolidators[0]);
        for (int i = 0; i < consolidator_count; i++)
            if (strcmp(consolidators[i].name, argv[2]) == 0) consolidator = &consolidators[i];
        if (consolidator == NULL) {
            fprintf(stderr, "Невідома функція консолідації: %s\n", argv[2]);
            print_usage(argv[0]);
            return 1;
        }
        first_task_arg = 3;
    }

    int request_pipe[2], response_pipe[2];
    if (pipe(request_pipe) < 0 || pipe(response_pipe) < 0) {
        perror("pipe");
        return 1;
    }

    fflush(stdout); /* щоб буфер не продублювався в дочірньому процесі */
    pid_t executive_pid = fork();
    if (executive_pid < 0) {
        perror("fork");
        return 1;
    }

    if (executive_pid == 0) {
        /* Виконавча частина: читає параметри, пише результати */
        close(request_pipe[1]);
        close(response_pipe[0]);
        executive_main(request_pipe[0], response_pipe[1]);
        _exit(0);
    }

    /* Інтерфейсна частина: пише параметри, читає результати */
    close(request_pipe[0]);
    close(response_pipe[1]);
    signal(SIGPIPE, SIG_IGN);

    struct ui_context ui;
    memset(&ui, 0, sizeof ui);
    ui.request_fd = request_pipe[1];
    ui.response_fd = response_pipe[0];
    ui.consolidator = consolidator;
    clock_gettime(CLOCK_MONOTONIC, &ui.stats.start_time);

    int exit_code = 0;
    if (request_dictionary(&ui) < 0) {
        fprintf(stderr, "Не вдалося отримати словник від виконавчої частини\n");
        exit_code = 1;
    } else {
        int status = (first_task_arg < argc)
                         ? run_argument_tasks(&ui, argv + first_task_arg, argc - first_task_arg)
                         : run_interactive(&ui);
        if (status < 0) {
            fprintf(stderr, "Втрачено зв'язок з виконавчою частиною\n");
            exit_code = 1;
        }
    }

    struct task_request quit_request;
    memset(&quit_request, 0, sizeof quit_request);
    quit_request.type = REQ_QUIT;
    write_all(ui.request_fd, &quit_request, sizeof quit_request);
    close(ui.request_fd);
    close(ui.response_fd);
    waitpid(executive_pid, NULL, 0);

    print_statistics(&ui);
    return exit_code;
}
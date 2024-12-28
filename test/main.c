#define _CRT_SECURE_NO_WARNINGS
// GTK and libwebsockets-based Tic-Tac-Toe game (asynchronous thread model, Windows supported)
#include <gtk/gtk.h>
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <glib.h>

#ifdef _WIN32
#include <windows.h>
#define usleep(x) Sleep((x) / 1000) // Replace usleep for Windows
#endif

#define MESSAGE_SIZE 512

static struct lws* web_socket = NULL;
static int interrupted = 0;
GtkWidget* buttons[3][3]; // Tic-Tac-Toe button grid
GtkWidget* status_label;
static GAsyncQueue* message_queue = NULL; // Ensure initialization

char room_id[50]; // Room ID
char user_id[50]; // User ID
char my_symbol = ' '; // User's symbol (X or O)
char current_turn = 'X'; // Current turn
int board[3][3] = { 0 }; // Board state (0: empty, 1: X, 2: O)
int players_connected = 0; // Track connected players

// WebSocket protocol initialization
static int callback_messenger(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len);
static struct lws_protocols protocols[] = {
    { "tic-tac-toe-protocol", callback_messenger, 0, MESSAGE_SIZE },
    { NULL, NULL, 0, 0 }
};

// Prompt for room ID and user ID
void prompt_for_room_and_user() {
    printf("Enter Room ID: ");
    fgets(room_id, sizeof(room_id), stdin);
    room_id[strcspn(room_id, "\n")] = 0; // Remove newline

    printf("Enter User ID: ");
    fgets(user_id, sizeof(user_id), stdin);
    user_id[strcspn(user_id, "\n")] = 0; // Remove newline
}

// Send message function
static int send_message(struct lws* wsi, const char* message) {
    printf("Sending message: %s\n", message); // 로그 추가
    size_t len = strlen(message) + strlen(room_id) + strlen(user_id) + 6;
    if (len > MESSAGE_SIZE - LWS_PRE) len = MESSAGE_SIZE - LWS_PRE;

    unsigned char buffer[LWS_PRE + MESSAGE_SIZE];
    unsigned char* p = &buffer[LWS_PRE];

    snprintf((char*)p, MESSAGE_SIZE, "[%s] %s: %s", room_id, user_id, message);
    return lws_write(wsi, p, strlen((char*)p), LWS_WRITE_TEXT);
}

// Button click handler
static void on_button_clicked(GtkWidget* widget, gpointer data) {
    if (players_connected < 2) return;

    int pos = GPOINTER_TO_INT(data);
    int row = pos / 3;
    int col = pos % 3;

    if (board[row][col] == 0 && current_turn == my_symbol) {
        board[row][col] = (my_symbol == 'X') ? 1 : 2;
        gtk_button_set_label(GTK_BUTTON(buttons[row][col]), (my_symbol == 'X') ? "X" : "O");
        char msg[50];
        snprintf(msg, sizeof(msg), "MOVE %d %d %c", row, col, my_symbol);
        printf("Button clicked: %s\n", msg); // 로그 추가
        send_message(web_socket, msg);
        current_turn = (my_symbol == 'X') ? 'O' : 'X';
    }
}

// WebSocket callback function
static int callback_messenger(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len) {
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        printf("WebSocket connected\n"); // 로그 추가
        g_async_queue_push(message_queue, g_strdup("CONNECTED"));
        send_message(wsi, "CONNECTED");
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        char* message = g_strdup_printf("%.*s", (int)len, (char*)in);
        printf("Received message: %s\n", message); // 로그 추가
        g_async_queue_push(message_queue, message);
        break;
    }
    case LWS_CALLBACK_CLOSED:
        printf("WebSocket closed\n"); // 로그 추가
        g_async_queue_push(message_queue, g_strdup("Connection closed."));
        interrupted = 1;
        break;
    default:
        break;
    }
    return 0;
}

// WebSocket connection initialization
static struct lws_context* initialize_websocket() {
    struct lws_context_creation_info context_info = { 0 };
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = protocols;
    context_info.options |= LWS_SERVER_OPTION_DISABLE_IPV6;

    struct lws_context* context = lws_create_context(&context_info);
    if (!context) {
        fprintf(stderr, "Failed to create WebSocket context\n");
        exit(1);
    }

    struct lws_client_connect_info connect_info = { 0 };
    connect_info.context = context;
    connect_info.address = "localhost";
    connect_info.port = 8080;
    connect_info.path = "/";
    connect_info.host = lws_canonical_hostname(context);
    connect_info.origin = "origin";
    connect_info.protocol = protocols[0].name;

    web_socket = lws_client_connect_via_info(&connect_info);
    if (!web_socket) {
        fprintf(stderr, "Failed to connect to WebSocket server\n");
        exit(1);
    }

    return context;
}

// WebSocket event loop
void* websocket_thread(void* arg) {
    struct lws_context* context = (struct lws_context*)arg;
    while (!interrupted) {
        lws_service(context, 100);
    }
    return NULL;
}

// Process incoming messages
static gboolean process_queue() {
    if (!message_queue) return TRUE;

    while (1) {
        char* message = (char*)g_async_queue_try_pop(message_queue);
        if (!message) break;

        printf("Processing message: %s\n", message); // 로그 추가
        int row = -1, col = -1;
        char symbol = ' ';
        char msg_content[256] = { 0 }; // Initialize buffer

        // Game start signal processing
        if (strstr(message, "Game starts!")) {
            printf("Game start signal received!\n"); // 로그 추가
            players_connected = 2;
            gtk_label_set_text(GTK_LABEL(status_label), "Game started! Waiting for moves.");
        }

        if (message) g_free(message);
    }
    return TRUE;
}

// Main function
int main(int argc, char** argv) {
    gtk_init(&argc, &argv);

    prompt_for_room_and_user();
    message_queue = g_async_queue_new();

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Tic-Tac-Toe");
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);

    GtkWidget* grid = gtk_grid_new();
    gtk_container_add(GTK_CONTAINER(window), grid);

    status_label = gtk_label_new("Waiting for second player...");
    gtk_grid_attach(GTK_GRID(grid), status_label, 0, 0, 3, 1);

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            buttons[i][j] = gtk_button_new_with_label(" ");
            gtk_grid_attach(GTK_GRID(grid), buttons[i][j], j, i + 1, 1, 1);
            g_signal_connect(buttons[i][j], "clicked", G_CALLBACK(on_button_clicked), GINT_TO_POINTER(i * 3 + j));
        }
    }
    gtk_widget_show_all(window);

    struct lws_context* context = initialize_websocket();
    pthread_t ws_thread;
    pthread_create(&ws_thread, NULL, websocket_thread, context);

    g_timeout_add(100, (GSourceFunc)process_queue, NULL);
    gtk_main();

    interrupted = 1;
    pthread_join(ws_thread, NULL);
    lws_context_destroy(context);
    return 0;
}
    
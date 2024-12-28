#define _CRT_SECURE_NO_WARNINGS
// GTK 및 libwebsockets 기반 GUI 메신저 (비동기 스레드 방식, Windows 지원)
#include <gtk/gtk.h>
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <glib.h>

#ifdef _WIN32
#include <windows.h>
#define usleep(x) Sleep((x) / 1000) // Windows용 usleep 대체
#endif

#define MESSAGE_SIZE 512

static struct lws* web_socket = NULL;
static int interrupted = 0;
GtkWidget* entry;
GtkWidget* text_view;
GtkTextBuffer* buffer;
static GAsyncQueue* message_queue;

char room_id[50]; // 방 ID 저장
char user_id[50]; // 사용자 ID 저장

// 메시지 전송 함수
static int send_message(struct lws* wsi, const char* message) {
    size_t len = strlen(message) + strlen(room_id) + strlen(user_id) + 6;
    if (len > MESSAGE_SIZE - LWS_PRE) len = MESSAGE_SIZE - LWS_PRE; // 크기 초과 방지

    unsigned char buffer[LWS_PRE + MESSAGE_SIZE];
    unsigned char* p = &buffer[LWS_PRE]; // 오프셋 적용

    snprintf((char*)p, MESSAGE_SIZE, "[%s] %s: %s", room_id, user_id, message); // 방 ID 및 사용자 ID 포함
    return lws_write(wsi, p, strlen((char*)p), LWS_WRITE_TEXT); // 텍스트 전송 플래그 적용
}

// GTK에 텍스트 삽입 함수
static gboolean insert_text_idle(gpointer data) {
    gtk_text_buffer_insert_at_cursor(buffer, (const gchar*)data, -1);
    g_free(data);
    return FALSE; // 작업 완료 후 제거
}

// 메시지 큐 처리
static gboolean process_queue() {
    while (1) { // 큐에 있는 모든 메시지 처리
        char* message = (char*)g_async_queue_try_pop(message_queue);
        if (!message) break; // 더 이상 메시지가 없으면 종료
        g_idle_add(insert_text_idle, message);
    }
    return TRUE; // 반복 호출 유지
}

// 콜백 함수
static int callback_messenger(struct lws* wsi, enum lws_callback_reasons reason,
    void* user, void* in, size_t len) {
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        g_async_queue_push(message_queue, g_strdup("Connected to server!\n"));
        send_message(wsi, "Hello Server!");
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        char* message = g_strdup_printf("%.*s\n", (int)len, (char*)in);
        // 방 ID 필터링
        if (strncmp(message + 1, room_id, strlen(room_id)) == 0) { // '[' 제외 후 ID 비교
            char* filtered_message = strchr(message, ']');
            if (filtered_message) {
                g_async_queue_push(message_queue, g_strdup(filtered_message + 2));
            }
        }
        g_free(message);
        break;
    }
    case LWS_CALLBACK_CLOSED:
        g_async_queue_push(message_queue, g_strdup("Connection closed\n"));
        interrupted = 1;
        break;
    default:
        break;
    }
    return 0;
}

// WebSocket 프로토콜 초기화
static struct lws_protocols protocols[] = {
    { "messenger-protocol", callback_messenger, 0, MESSAGE_SIZE },
    { NULL, NULL, 0, 0 }
};

// 버튼 클릭 이벤트 처리
static void on_send_button_clicked(GtkWidget* widget, gpointer data) {
    const char* message = gtk_entry_get_text(GTK_ENTRY(entry));
    if (web_socket && message && strlen(message) > 0) {
        // 내가 보낸 메시지를 UI에 추가
        char* display_message = g_strdup_printf("Me (%s): %s\n", user_id, message);
        g_async_queue_push(message_queue, display_message);

        // 서버로 메시지 전송
        send_message(web_socket, message);
        gtk_entry_set_text(GTK_ENTRY(entry), "");
    }
}

// GTK 종료 처리
static void on_destroy(GtkWidget* widget, gpointer data) {
    interrupted = 1; // 스레드 종료 신호 설정
    if (web_socket) {
        lws_close_reason(web_socket, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
        lws_cancel_service(lws_get_context(web_socket)); // 서비스 종료 신호
    }
}

// WebSocket 스레드 루프
void* websocket_thread(void* arg) {
    struct lws_context* context = (struct lws_context*)arg;
    while (!interrupted) {
        lws_service(context, 1); // 짧은 타임아웃으로 빠른 응답 보장
        usleep(1000); // CPU 점유율 과다 방지
    }
    return NULL; // 스레드 종료
}

// 방 및 사용자 ID 선택 화면
static void create_room_and_user_selection_screen(GtkWidget* window) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Enter Room and User ID", GTK_WINDOW(window),
        GTK_DIALOG_MODAL, "Enter", GTK_RESPONSE_ACCEPT,
        "Cancel", GTK_RESPONSE_CANCEL, NULL);
    GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* entry_room = gtk_entry_new();
    GtkWidget* entry_user = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(content_area), gtk_label_new("Room ID:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content_area), entry_room, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content_area), gtk_label_new("User ID:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content_area), entry_user, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        strncpy(room_id, gtk_entry_get_text(GTK_ENTRY(entry_room)), sizeof(room_id) - 1);
        strncpy(user_id, gtk_entry_get_text(GTK_ENTRY(entry_user)), sizeof(user_id) - 1);
    }
    else {
        exit(0);
    }
    gtk_widget_destroy(dialog);
}
// WebSocket 연결 초기화 함수
static void initialize_websocket_connection(GtkWidget* window) {
    // WebSocket 설정
    struct lws_context_creation_info context_info = { 0 };
    context_info.port = CONTEXT_PORT_NO_LISTEN; // 클라이언트 모드
    context_info.protocols = protocols;
    context_info.options |= LWS_SERVER_OPTION_DISABLE_IPV6; // IPv6 비활성화

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

    message_queue = g_async_queue_new(); // 메시지 큐 생성

    // WebSocket 스레드 시작
    pthread_t ws_thread;
    pthread_create(&ws_thread, NULL, websocket_thread, context);

    // 메시지 큐 처리 루프 추가
    g_timeout_add(100, (GSourceFunc)process_queue, NULL);
}


int main(int argc, char** argv) {
    gtk_init(&argc, &argv);

    // 메인 윈도우 생성
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "GTK Messenger");
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);
    gtk_widget_set_size_request(window, 400, 300);

    // 방 및 사용자 ID 입력 화면
    create_room_and_user_selection_screen(window);

    // UI 설정
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    text_view = gtk_text_view_new();
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), text_view, TRUE, TRUE, 0);

    entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(vbox), entry, FALSE, FALSE, 0);

    GtkWidget* send_button = gtk_button_new_with_label("Send");
    gtk_box_pack_start(GTK_BOX(vbox), send_button, FALSE, FALSE, 0);

    g_signal_connect(send_button, "clicked", G_CALLBACK(on_send_button_clicked), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), NULL);

    gtk_widget_show_all(window); // GUI 표시

    // WebSocket 초기화
    initialize_websocket_connection(window);

    // GTK 메인 루프 실행
    gtk_main();

    return 0;
}

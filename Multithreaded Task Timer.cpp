#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <memory>
#include <windows.h>

#ifdef _WIN32
#pragma execution_character_set("utf-8")
#endif


using Clock = std::chrono::steady_clock;

struct TimerInfo {
    int id;                                     // Идентификатор таймера.
    std::string label;                          // Имя задачи.
    std::chrono::seconds total;                 // Длительность таймера.
    Clock::time_point start;
    Clock::time_point end;

    std::shared_ptr<std::atomic<bool>> cancelled; // true, если таймер отменён.
    std::shared_ptr<std::atomic<bool>> finished;  // true, если таймер успешно завершился.
    std::thread worker;                           // Поток, отсчитывающий данный таймер.
};

// Глобальный флаг работы приложения. Используется для мягкого завершения всех потоков.
std::atomic<bool> g_running{ true };

// Мьютекс для синхронизации вывода в консоль.
std::mutex g_cout_mutex;

// Мьютекс для защиты контейнера с таймерами.
std::mutex g_timers_mutex;

// Хранилище всех таймеров (как активных, так и завершённых/отменённых).
std::vector<TimerInfo> g_timers;

// Вывод в консоль. 
void safe_print(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_cout_mutex);
    std::cout << msg << std::flush;
}

// Корректный вид для чтения
std::string format_duration(std::chrono::seconds s) {
    auto total = s.count();
    if (total < 0) total = 0;
    int m = static_cast<int>(total / 60);
    int sec = static_cast<int>(total % 60);
    std::ostringstream oss;
    if (m > 0) {
        oss << m << "m";
        if (sec > 0) oss << sec << "s";
    }
    else {
        oss << sec << "s";
    }
    return oss.str();
}

// Функция, выполняющаяся в отдельном потоке для конкретного таймера.
// Отсчитывает время до end, реагирует на отмену и глобальное завершение.
void timer_thread_func(
    int id,
    std::string label,
    Clock::time_point /*start*/,  // start не используется внутри, но хранится в TimerInfo.
    Clock::time_point end,
    std::shared_ptr<std::atomic<bool>> cancelled,
    std::shared_ptr<std::atomic<bool>> finished
) {
    using namespace std::chrono;

    // Проверяем, не закончился ли таймер,
    // не отменён ли он и не завершено ли приложение.
    while (g_running.load(std::memory_order_relaxed) &&
        !cancelled->load(std::memory_order_relaxed)) {

        auto now = Clock::now();
        if (now >= end) {
            break;
        }

        auto remaining = duration_cast<seconds>(end - now);
        if (remaining <= seconds(0)) {
            break;
        }

        // Спим шагами, чтобы быстрее реагировать на отмену/выход.
        auto step = remaining > seconds(1) ? seconds(1) : remaining;
        std::this_thread::sleep_for(step);
    }

    // Если приложение останавливается или таймер отменён — выходим тихо.
    if (!g_running.load(std::memory_order_relaxed) ||
        cancelled->load(std::memory_order_relaxed)) {
        return;
    }

    // Помечаем таймер как успешно завершённый.
    finished->store(true, std::memory_order_relaxed);

    // Сообщаем о завершении.
    std::ostringstream oss;
    oss << "[DONE]  #" << id << " \"" << label << "\"\n";
    safe_print(oss.str());
}

// Создаёт новый таймер и запускет для него поток.
// Возвращает id созданного таймера или -1 в случае ошибки.
int add_timer(std::chrono::seconds duration, const std::string& label) {
    if (duration <= std::chrono::seconds(0)) {
        safe_print("Длительность должна быть > 0.\n");
        return -1;
    }

    // счётчик id
    static int next_id = 1;

    TimerInfo t;
    t.id = next_id++;
    t.label = label.empty() ? "Без названия" : label;
    t.total = duration;
    t.start = Clock::now();
    t.end = t.start + duration;
    t.cancelled = std::make_shared<std::atomic<bool>>(false);
    t.finished = std::make_shared<std::atomic<bool>>(false);

    {
        // Под защитой мьютекса добавляем таймер в общий контейнер
        // и сразу привязываем к нему поток.
        std::lock_guard<std::mutex> lock(g_timers_mutex);
        t.worker = std::thread(
            timer_thread_func,
            t.id,
            t.label,
            t.start,
            t.end,
            t.cancelled,
            t.finished
        );
        g_timers.emplace_back(std::move(t));
    }

    std::ostringstream oss;
    oss << "[ADD]  #" << (next_id - 1)
        << " \"" << (label.empty() ? "Без названия" : label)
        << "\" на " << format_duration(duration) << "\n";
    safe_print(oss.str());

    return next_id - 1;
}

// Выводит список всех таймеров и их состояние.
void list_timers() {
    std::lock_guard<std::mutex> lock(g_timers_mutex);

    if (g_timers.empty()) {
        safe_print("Активных/завершённых таймеров нет.\n");
        return;
    }

    std::ostringstream oss;
    oss << "Таймеры:\n";

    auto now = Clock::now();
    for (const auto& t : g_timers) {
        bool cancelled = t.cancelled->load(std::memory_order_relaxed);
        bool finished = t.finished->load(std::memory_order_relaxed);

        oss << "  #" << t.id << " \"" << t.label << "\" ";

        if (cancelled) {
            oss << "[CANCELLED]";
        }
        else if (finished) {
            oss << "[DONE]";
        }
        else {
            if (now >= t.end) {
                // таймер уже должен был сработать, но поток ещё не отметил finished.
                oss << "[PENDING DONE]";
            }
            else {
                auto remaining = std::chrono::duration_cast<std::chrono::seconds>(t.end - now);
                oss << "[RUNNING, осталось " << format_duration(remaining) << "]";
            }
        }

        oss << "\n";
    }

    safe_print(oss.str());
}

// Отмена конкретного таймера по id.
// Для простоты join вызывается под мьютексом;
void cancel_timer(int id) {
    std::lock_guard<std::mutex> lock(g_timers_mutex);

    for (auto& t : g_timers) {
        if (t.id == id) {
            if (t.cancelled->load() || t.finished->load()) {
                safe_print("Таймер уже завершён или отменён.\n");
                return;
            }

            t.cancelled->store(true);
            if (t.worker.joinable()) {
                t.worker.join();
            }

            std::ostringstream oss;
            oss << "[CANCEL] #" << id << " \"" << t.label << "\"\n";
            safe_print(oss.str());
            return;
        }
    }

    safe_print("Таймер с таким id не найден.\n");
}

// Останавливает приложение и корректно завершает все таймеры.
// Вызывается при выходе из main и из обработчика сигнала.
void shutdown_all() {
    g_running.store(false);

    std::lock_guard<std::mutex> lock(g_timers_mutex);

    // Запрашиваем отмену всех таймеров.
    for (auto& t : g_timers) {
        t.cancelled->store(true);
    }

    // Дожидаемся завершения всех потоков.
    for (auto& t : g_timers) {
        if (t.worker.joinable()) {
            t.worker.join();
        }
    }
}

// Обработчик SIGINT (Ctrl+C).
// Позволяет корректно завершить приложение и все потоки.
void signal_handler(int) {
    safe_print("\nПолучен сигнал, завершаем...\n");
    shutdown_all();
    std::exit(0);
}

// Вывод краткой справки по доступным командам.
void print_help() {
    safe_print(
        "Команды:\n"
        "  help                          - показать помощь\n"
        "  add <минуты> <название>      - добавить таймер\n"
        "  pomodoro <название>          - 25 мин работы + 5 мин перерыв\n"
        "  list                          - список таймеров\n"
        "  cancel <id>                   - отменить таймер\n"
        "  exit                          - выйти\n"
    );
}

int main() {

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif


    // обработчик Ctrl+C 
    std::signal(SIGINT, signal_handler);

    safe_print("MultiTimer (многопоточный C++ таймер)\n");
    print_help();

    std::string line;
    while (g_running.load()) {
        {
            // Выводим приглашение к вводу. Под мьютексом, чтобы не смешивать с другими выводами.
            std::lock_guard<std::mutex> lock(g_cout_mutex);
            std::cout << "> " << std::flush;
        }

        if (!std::getline(std::cin, line)) {
            // EOF или ошибка ввода — выходим из цикла.
            break;
        }

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd.empty()) {
            continue;
        }

        if (cmd == "help") {
            print_help();
        }
        else if (cmd == "add") {
            int minutes;
            iss >> minutes;
            if (!iss || minutes <= 0) {
                safe_print("Использование: add <минуты> <название>\n");
                continue;
            }

            std::string label;
            std::getline(iss, label);
            if (!label.empty() && label[0] == ' ')
                label.erase(0, 1);

            add_timer(std::chrono::seconds(minutes * 60), label);
        }
        else if (cmd == "pomodoro") {
            std::string label;
            std::getline(iss, label);
            if (!label.empty() && label[0] == ' ')
                label.erase(0, 1);
            if (label.empty())
                label = "Pomodoro";

            // Pomodoro: 25 минут работы + 5 минут перерыва.
            add_timer(std::chrono::seconds(25 * 60), "Work: " + label);
            add_timer(std::chrono::seconds(5 * 60), "Break after: " + label);
        }
        else if (cmd == "list") {
            list_timers();
        }
        else if (cmd == "cancel") {
            int id;
            iss >> id;
            if (!iss) {
                safe_print("Использование: cancel <id>\n");
                continue;
            }
            cancel_timer(id);
        }
        else if (cmd == "exit") {
            break;
        }
        else {
            safe_print("Неизвестная команда. Напишите help.\n");
        }
    }

    // Завершение всех потоков при выходе
    shutdown_all();
    safe_print("Выход.\n");
    return 0;
}

#include <cdr/base/concurrent/rcu.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace std::chrono_literals;

TEST(RcuVariable, Sanity) {
    cdr::RcuVariable<int> variable(123);
    EXPECT_EQ(*variable.Read(), 123);
}

// Тест 1: Множество потоков читают значение одновременно
TEST(RcuVariable, ConcurrentReads) {
    cdr::RcuVariable<int> var(42);
    constexpr int kNumThreads = 10;
    constexpr int kNumIterations = 10000;

    auto reader_task = [&var]() {
        for (int i = 0; i < kNumIterations; ++i) {
            auto ptr = var.Read();
            EXPECT_EQ(*ptr, 42);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back(reader_task);
    }

    for (auto& t : threads) {
        t.join();
    }
}

// Тест 2: Конкурентное обновление (Read-Modify-Write)
// Проверяем, что StartWrite() корректно работает как критическая секция
TEST(RcuVariable, ConcurrentReadModifyWrite) {
    cdr::RcuVariable<int> var(0);
    constexpr int kNumThreads = 8;
    constexpr int kNumIncrementsPerThread = 5000;

    auto writer_task = [&var]() {
        for (int i = 0; i < kNumIncrementsPerThread; ++i) {
            auto wptr = var.StartWrite();
            *wptr += 1;
            wptr.Commit();
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back(writer_task);
    }

    for (auto& t : threads) {
        t.join();
    }

    // Все инкременты должны быть учтены (без гонок данных)
    EXPECT_EQ(*var.Read(), kNumThreads * kNumIncrementsPerThread);
}

// Тест 3: Читатели читают старые/новые значения, пока писатель обновляет переменную.
// Это проверяет механизм утилизации старых Snapshot-ов (ScanRetiredList).
TEST(RcuVariable, ReadersAndWriters) {
    cdr::RcuVariable<int> var(0);
    std::atomic<bool> stop_flag{false};

    // Читатели просто непрерывно читают и проверяют,
    // что значение монотонно возрастает или остается прежним.
    auto reader_task = [&var, &stop_flag]() {
        int last_seen = 0;
        while (!stop_flag.load(std::memory_order_acquire)) {
            auto ptr = var.Read();
            int current = *ptr;
            EXPECT_GE(current, last_seen); // Значение не может откатиться назад
            last_seen = current;

            // Немного нагружаем процессор для имитации работы с данными
            std::this_thread::yield();
        }
    };

    constexpr int kNumReaders = 6;
    std::vector<std::thread> readers;
    for (int i = 0; i < kNumReaders; ++i) {
        readers.emplace_back(reader_task);
    }

    // Писатель постоянно перезаписывает значение
    constexpr int kNumWrites = 10000;
    for (int i = 1; i <= kNumWrites; ++i) {
        var.Assign(i);
        // Небольшая задержка, чтобы читатели успевали захватывать указатели
        if (i % 100 == 0) {
            std::this_thread::sleep_for(1ms);
        }
    }

    // Дожидаемся завершения записи и останавливаем читателей
    stop_flag.store(true, std::memory_order_release);
    for (auto& t : readers) {
        t.join();
    }

    EXPECT_EQ(*var.Read(), kNumWrites);
}

// Тест 4: Защита от преждевременного удаления (Use-After-Free)
TEST(RcuVariable, SnapshotIsPreservedWhileRead) {
    cdr::RcuVariable<std::string> var("initial");

    // 1. Читатель получает доступ к текущему значению (сохраняет TickRAII защиту)
    auto read_ptr = var.Read();
    EXPECT_EQ(*read_ptr, "initial");

    // 2. Писатель перезаписывает данные.
    // "initial" помещается в retired_list_, но НЕ удаляется, так как read_ptr жив.
    var.Assign("updated_1");
    var.Assign("updated_2");
    var.Assign("updated_3");

    // Принудительно вызываем Cleanup, чтобы проверить, что он не удалит активный Snapshot
    var.Cleanup();

    // 3. Данные по старому указателю все еще должны быть валидны
    EXPECT_EQ(*read_ptr, "initial");

    // 4. Новый читатель должен видеть самые свежие данные
    auto read_ptr2 = var.Read();
    EXPECT_EQ(*read_ptr2, "updated_3");
}

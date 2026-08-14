# Утилита хранилища. Одна только программа: сами операции переехали в
# zametti-core/store, потому что зовут их и наборы, и окно, и она.
#
# QGuiApplication, а не QCoreApplication: ядро зависит от QtGui (там живёт
# QTextDocument), и пережатие вложений разжимает картинки. Окон утилита не
# открывает — платформу она ставит offscreen сама, до создания приложения.
add_executable(zametti-store main.cpp)
target_link_libraries(zametti-store PRIVATE zametti-core zametti-code-rules)

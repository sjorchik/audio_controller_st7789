Хост-тест E3 (лише ПК, у збірку прошивки не входить).
Запуск з кореня репозиторію:  sh test_host_e3/run.sh   (потрібен g++ >= 9, C++17)
stubs/ — заглушки: Arduino.h, esp_timer.h, freertos/*, а також config/features.h, config/station_level_config.h,
config/balance_config.h, audio/vu_source.h (ці чотири справжні файли чату не прикріплювались; стоять ПЕРЕД src у -I і
лише на час тесту підміняють справжні).

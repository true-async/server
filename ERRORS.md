1. Комментари в коде говно!!!! Убрать лишние комментарии, которые не несут смысловой нагрузки!
2. http_compression_message.h - отдельный модуль ради двуз функций? оверинженеринг!!!
    if (req == NULL || req->headers == NULL) {
        return false;
   }

3. в коде много проверок, которые скорее всего не нужны
4. имена параметров говно:     zval *ct = zend_hash_str_find(req->headers, "content-type", - что такое ct?
5. grpc_request_is_grpc и grpc_request_is_grpc_web - явное дублирование кода. и не только!

6. Максимально grpc_request_mode не эффективный код!!!! Можно было бы сперва получить строку потом сравнить в switch
7. grpc_web_text_decode - можно ли предсказать буфер? примерно?
8. Куча smart_str_appendl
9.     switch (s[n - 1]) {
        case 'H': unit_ns = 3600ULL * 1000000000ULL; break;   /* hours   */
        case 'M': unit_ns =   60ULL * 1000000000ULL; break;   /* minutes */
        case 'S': unit_ns =          1000000000ULL;  break;   /* seconds */
        case 'm': unit_ns =             1000000ULL;  break;   /* millis  */
        case 'u': unit_ns =                1000ULL;  break;   /* micros  */
        case 'n': unit_ns =                   1ULL;  break;   /* nanos   */
        default:  return 0;
пахнет отдельной общей функцией

10. grpc_message_inflate можно вообще в коде не определять и не вызывать если нет поддержки сжатия
11. 
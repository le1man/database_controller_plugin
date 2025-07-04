#include <Server/Components/Pawn/pawn.hpp>
#include <Server/Components/Pawn/Impl/pawn_natives.hpp>
#include <Server/Components/Pawn/Impl/pawn_impl.hpp>

#include <sdk.hpp>

//#include <iostream>// for tests
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <map>
#include <utility>
#include <tuple>
#include <random>
#include <unordered_set>
//#include <mutex>
//#include <shared_mutex>


//#ifdef _WIN32
//#include <Windows.h>
//#endif

#include <curl/curl.h>
#include <json.hpp>

using njson = nlohmann::json;

std::string url_host;
std::string api_key_for_host;
std::string sharded_url_host;
std::string sharded_login;
std::string sharded_pass;
//static std::mutex config_mutex;
//______________________________________LOAD CONFIG
void LoadConfig() {
    //std::lock_guard<std::mutex> lock(config_mutex);

    std::ifstream file("database_controller.json");
    if (!file.is_open()) {
        throw std::runtime_error("Не удалось открыть файл конфигурации database_controller.json");
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string file_content = buffer.str();

    const njson content_converted = njson::parse(file_content);

    if (content_converted.contains("url_host") && content_converted.contains("api_key_for_host")) {
        url_host = content_converted["url_host"].get<std::string>();
        api_key_for_host = content_converted["api_key_for_host"].get<std::string>();
    }

    if (content_converted.contains("sharded_url_host") && content_converted.contains("sharded_login") && content_converted.contains("sharded_pass")) {
        sharded_url_host = content_converted["sharded_url_host"].get<std::string>();
        sharded_login = content_converted["sharded_login"].get<std::string>();
        sharded_pass = content_converted["sharded_pass"].get<std::string>();
    }
}
//_________________________________________________

//______________________________________Threads
//#ifdef _WIN32
//namespace ThreadControl {
//
//    template<typename F, typename... Args>
//    struct ThreadData {
//        std::decay_t<F> func;
//        std::tuple<std::decay_t<Args>...> args;
//
//        ThreadData(F&& f, Args&&... a)
//            : func(std::forward<F>(f))
//            , args(std::forward<Args>(a)...)
//        {
//        }
//    };
//
//    template<typename F, typename... Args>
//    DWORD WINAPI ThreadProc(LPVOID lpParameter) {
//        auto* data = static_cast<ThreadData<F, Args...>*>(lpParameter);
//        // распаковываем кортеж и вызываем func(args...)
//        try {
//            std::apply(data->func, data->args);
//        }
//        catch (const std::exception& e) {
//            //logging
//        }
//        catch (...) { /* swallow */ }
//        delete data;               // освобождаем память
//        return 0;
//    }
//
//    template<typename F, typename... Args>
//    HANDLE CreateThreadEx(F&& func, Args&&... args) {
//        using Data = ThreadData<F, Args...>;
//        auto* data = new Data(
//            std::forward<F>(func),
//            std::forward<Args>(args)...
//        );
//
//        HANDLE h = CreateThread(
//            /*attr=*/   nullptr,
//            /*stack=*/  0,
//            /*start=*/  ThreadProc<F, Args...>,
//            /*param=*/  data,
//            /*flags=*/  0,
//            /*tid=*/    nullptr
//        );
//        if (!h) {
//            delete data; return nullptr;  // в случае ошибки чистим
//        }
//        CloseHandle(h);
//        return h;
//    }
//};
//#endif
//_____________________________________________

//______________________________________CURL
class CurlClient {
public:
    CurlClient(const std::string& baseUrl,
        const std::string& apiKey = "",
        const std::string& user = "",
        const std::string& password = "")
        : curl_(nullptr)
        , headers_(nullptr)
        , baseUrl_(baseUrl)
        , user_(user)
        , password_(password)
    {
        static bool globalInitDone = false;
        if (!globalInitDone) {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
                throw std::runtime_error("curl_global_init failed");
            globalInitDone = true;
        }

        curl_ = curl_easy_init();
        if (!curl_)
            throw std::runtime_error("curl_easy_init failed");

        headers_ = curl_slist_append(headers_, "accept: application/json");
        if (!apiKey.empty())
            headers_ = curl_slist_append(headers_, ("x-api-key: " + apiKey).c_str());
        headers_ = curl_slist_append(headers_, "Content-Type: application/json");

        if (!user_.empty() && !password_.empty()) {
            userpwd_ = user_ + ":" + password_;
            curl_easy_setopt(curl_, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl_, CURLOPT_USERPWD, userpwd_.c_str());
        }

        // Общие опции
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    }

    ~CurlClient() {
        if (headers_) curl_slist_free_all(headers_);
        if (curl_) curl_easy_cleanup(curl_);
    }

    std::pair<std::string, long> postJson(const std::string& endpoint, const std::string& jsonBody) {
        std::string response;
        long httpCode = 0;
        prepareRequest(endpoint, response);

        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "POST");
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, jsonBody.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));

        perform(response, httpCode);
        return { response, httpCode };
    }

    std::pair<std::string, long> get(const std::string& endpoint) {
        std::string response;
        long httpCode = 0;
        prepareRequest(endpoint, response);

        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        perform(response, httpCode);
        return { response, httpCode };
    }

    std::pair<std::string, long> putForm(const std::string& endpoint, const std::string& formBody) {
        std::string response;
        long httpCode = 0;
        prepareRequest(endpoint, response);

        // заменить Content-Type
        struct curl_slist* putHeaders = curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded");
        putHeaders = curl_slist_append(putHeaders, "accept: application/json");

        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, putHeaders);
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, formBody.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(formBody.size()));

        perform(response, httpCode);
        curl_slist_free_all(putHeaders);
        return { response, httpCode };
    }

    std::pair<std::string, long> del(const std::string& endpoint) {
        std::string response;
        long httpCode = 0;
        prepareRequest(endpoint, response);

        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
        perform(response, httpCode);
        return { response, httpCode };
    }

private:
    CURL* curl_;
    struct curl_slist* headers_;
    std::string baseUrl_;
    std::string user_;
    std::string password_;
    std::string userpwd_;

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* resp = static_cast<std::string*>(userp);
        resp->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    void prepareRequest(const std::string& endpoint, std::string& responseStorage) {
        std::string fullUrl = baseUrl_ + endpoint;
        responseStorage.clear();

        curl_easy_setopt(curl_, CURLOPT_URL, fullUrl.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &responseStorage);
    }

    void perform(std::string& response, long& httpCode) {
        CURLcode res = curl_easy_perform(curl_);
        if (res != CURLE_OK) {
            throw std::runtime_error(
                std::string("curl_easy_perform failed: ") + curl_easy_strerror(res));
        }
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &httpCode);
    }
};
//__________________________________________

//______________________________________Singleton
template <class T>
class Singleton
{
protected:
    static T* m_Instance;

public:
    Singleton() = default;
    virtual ~Singleton() = default;

    inline static T* Get()
    {
        if (m_Instance == nullptr)
            m_Instance = new T;

        return m_Instance;
    }

    inline static void Destroy()
    {
        if (m_Instance != nullptr)
        {
            delete m_Instance;
            m_Instance = nullptr;
        }
    }
};

template <class T>
T* Singleton<T>::m_Instance = nullptr;
//_______________________________________________

//______________________________________ОСНОВНАЯ ЛОГИКА СХЕМ
namespace Schemas {
    //static std::mutex schema_mutex;

    // Типы баз данных
    enum class DBType { SHARDED, MONGO };

    // Типы полей
    enum class FieldType { STRING, INT, UINT, FLOAT, DOUBLE, BOOL };

    // Описание одного поля
    struct FieldSchema {
        std::string name;
        FieldType type;
        // для строк: максимальная длина (0 — неограничено)
        // для чисел: ширина в битах (например, 32 для int32)
        size_t width;
    };

    // Описание одной “таблицы” или коллекции
    struct TableSchema {
        std::string name;                   // имя таблицы/коллекции
        std::vector<FieldSchema> fields;    // поля
    };

    // Описание одной базы (или коллекции в Mongo)
    struct DatabaseSchema {
        DBType type;
        std::string name;
        TableSchema table;  // здесь по заданию одна “таблица” на блок схемы
    };

    // Вспомогательные функции для обрезки пробелов
    static inline void ltrim(std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(),
            [](unsigned char ch) { return !std::isspace(ch); }));
    }
    static inline void rtrim(std::string& s) {
        s.erase(std::find_if(s.rbegin(), s.rend(),
            [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
    }
    static inline void trim(std::string& s) {
        ltrim(s);
        rtrim(s);
    }

    // Преобразуем строку типа в FieldType и битовую ширину
    static FieldSchema parseFieldType(const std::string& typeStr, size_t length) {
        FieldSchema fs;
        fs.width = length;
        if (typeStr == "string") {
            fs.type = FieldType::STRING;
        }
        else if (typeStr.rfind("int", 0) == 0) {
            fs.type = FieldType::INT;
            // извлечь число после “int”
            fs.width = std::stoul(typeStr.substr(3));
        }
        else if (typeStr.rfind("uint", 0) == 0) {
            fs.type = FieldType::UINT;
            // после “uint_” или “uint”
            auto pos = typeStr.find_first_of("0123456789");
            if (pos != std::string::npos)
                fs.width = std::stoul(typeStr.substr(pos));
        }
        else if (typeStr == "float") {
            fs.type = FieldType::FLOAT;
        }
        else if (typeStr == "double") {
            fs.type = FieldType::DOUBLE;
        }
        else if (typeStr == "bool") {
            fs.type = FieldType::BOOL;
            fs.width = 1;
        }
        else {
            throw std::runtime_error("Неизвестный тип поля: " + typeStr);
        }
        return fs;
    }

    // Парсинг файла-схемы
    std::vector<DatabaseSchema> parseSchemaFile(const std::string& filename) {
        //std::lock_guard<std::mutex> lock(schema_mutex);
        std::ifstream in(filename);
        if (!in) throw std::runtime_error("Не удалось открыть файл: " + filename);

        std::vector<DatabaseSchema> result;
        DatabaseSchema* currentDB = nullptr;
        bool shardedSeen = false;
        std::string line;

        while (std::getline(in, line)) {
            trim(line);
            if (line.empty()) continue;

            // Новый блок [TYPE:name]
            if (line.front() == '[' && line.back() == ']') {
                auto colon = line.find(':');
                if (colon == std::string::npos)
                    throw std::runtime_error("Неправильный заголовок блока: " + line);

                std::string typeStr = line.substr(1, colon - 1);
                std::string nameStr = line.substr(colon + 1, line.size() - colon - 2);
                trim(typeStr); trim(nameStr);

                DBType dbt;
                if (typeStr == "SHARDED") {
                    if (shardedSeen)
                        throw std::runtime_error("Нельзя объявлять более одного SHARDED:");
                    dbt = DBType::SHARDED;
                    shardedSeen = true;
                }
                else if (typeStr == "MONGO") {
                    dbt = DBType::MONGO;
                }
                else {
                    throw std::runtime_error("Неизвестный тип БД: " + typeStr);
                }

                result.push_back({ dbt, nameStr, {nameStr, {}} });
                currentDB = &result.back();
            }
            // Поле
            else if (line.front() == '#') {
                if (!currentDB)
                    throw std::runtime_error("Поле встречено вне блока БД: " + line);

                // убрать комментарий
                auto cpos = line.find("//");
                if (cpos != std::string::npos)
                    line.erase(cpos);

                // #name:length type
                line.erase(0, 1);  // удаляем '#'
                trim(line);
                auto colon = line.find(':');
                auto space = line.find(' ', colon + 1);
                if (colon == std::string::npos || space == std::string::npos)
                    throw std::runtime_error("Неправильная строка поля: " + line);

                std::string fname = line.substr(0, colon);
                std::string lenStr = line.substr(colon + 1, space - colon - 1);
                std::string typeStr = line.substr(space + 1);
                trim(fname); trim(lenStr); trim(typeStr);

                size_t length = 0;
                if (lenStr == "*") {
                    length = 0; // 0 — неограничено для string; для остальных — будет заменено при парсинге типа
                }
                else {
                    length = std::stoul(lenStr);
                }

                FieldSchema fs = parseFieldType(typeStr, length);
                fs.name = fname;
                currentDB->table.fields.push_back(fs);
            }
            // иначе пропустить
        }

        return result;
    }
};
//__________________________________________________________

class databasePawnExtension : public Singleton<databasePawnExtension>{

private:
    //mutable std::shared_mutex data_mutex_;
    //using ReadLock = std::shared_lock<std::shared_mutex>;
    //using WriteLock = std::unique_lock<std::shared_mutex>;

    IPawnComponent* pawn_;

    int data_ = 0;
    std::map<int, std::vector<Schemas::DatabaseSchema>> storage_;

    std::unordered_map<int32_t, int> UUID_Call_; // uuid, id_database from storage_
    std::unordered_map<int32_t, std::vector<njson>> call_data_; // uuid, json scheme for request
    std::unordered_map<int32_t, std::vector<njson>> filter_data_; // uuid, json scheme for update request
    std::unordered_map<int32_t, std::vector<njson>> projection_field_; // uuid, json scheme for exclude/include fields in find
    std::unordered_map<int32_t, std::vector<njson>> find_response_; // response find command

    // FOR SHARDED
    std::unordered_map<int32_t, std::string> call_sharded_id_;
    std::unordered_map<int32_t, std::vector<njson>> add_sharded_data_;
    std::unordered_map<int32_t, std::vector<std::string>> get_sharded_fields_;
    std::unordered_map<int32_t, njson> find_sharded_data_;
    std::unordered_map<int32_t, std::vector<njson>> update_sharded_data_;
    std::unordered_map<int32_t, njson> find_get_sharded_response_;
    std::unordered_map<int32_t, std::string> add_ref_;
    
    // payload
    std::unordered_map<int32_t, std::vector<njson>> payload_;

    // Вспомогательная функция для проверки соответствия типов
    bool isTypeMatch(Schemas::FieldType ft, const njson& val) {
        using vt = njson::value_t;
        if (val.is_null() || val.is_array() || val.is_object())
            return false;

        switch (ft) {
            case Schemas::FieldType::STRING:  return val.is_string();
            case Schemas::FieldType::INT:     return val.is_number_integer();
            case Schemas::FieldType::UINT:    return val.is_number_integer();
            case Schemas::FieldType::FLOAT:
            case Schemas::FieldType::DOUBLE:  return val.is_number_float();
            case Schemas::FieldType::BOOL: return val.is_boolean();
        }
        return false;
    }

    //https://music.yandex.ru/album/6360227/track/47039673
    //{
    //    "documents": [
    //    {
    //        ///0 (doc_id)
    //    },
    //    {
    //        ///1
    //    },
    //    {
    //        ///2
    //    }//...
    //    ]
    //}
    //resp["documents"][doc_id]
    // дальше его надо как то парсить?!
    // 
    // 
    // ответ был неожиданным, chatgpt справилась)
    
    // для чекера доступности ключа при возврате
    bool checkFieldAvailability(const njson& resp, const int& doc_id, const std::string& field) {
        if (resp.contains("documents") && resp["documents"].is_array() && doc_id < resp["documents"].size()) {
            if (resp["documents"][doc_id].is_object() && resp["documents"][doc_id].contains(field)) {
                return true;
            }
        }
        return false;
    }

    void clear_data_() {
        data_ = 0;
        storage_.clear();
        UUID_Call_.clear();
        call_data_.clear();
        filter_data_.clear();
        projection_field_.clear();
        find_response_.clear();

        // SHARDED
        call_sharded_id_.clear();
        add_sharded_data_.clear();
        get_sharded_fields_.clear();
        find_sharded_data_.clear();
        update_sharded_data_.clear();
        find_get_sharded_response_.clear();
        add_ref_.clear();

        payload_.clear();
    }

    int check_uuid_(const int32_t& UUID, const int& DB) {
        // 0) Проверим соответствует ли указанный uuid нашей базе
        auto it = UUID_Call_.find(UUID);
        if (it == UUID_Call_.end() || it->second != DB)
            return -4; // Указана неверная БД, для которой создавался запрос
        else return 0;
    }

    std::pair<int, std::vector<Schemas::DatabaseSchema>::const_iterator> check_db_(
        const int& DB,
        const std::string& collection_name,
        const Schemas::DBType& dbtype
    ) {
        // 1) Найти нужную БД один раз
        const auto& dbs = storage_[DB];
        auto db_it = std::find_if(dbs.begin(), dbs.end(),
            [&](auto const& db) {
                return db.type == dbtype
                    && db.name == collection_name;
            });
        if (db_it == dbs.end())
            return { -3, db_it };  // коллекция не найдена
        else return { 0 , db_it };
    }

    int parse_json_scheme_(
        const njson& json, const std::vector<Schemas::DatabaseSchema>::const_iterator& db_it, 
        const bool& simplified = false
    ) {
        // 2) Перебирать только ключи JSON-а, искать поле и проверять тип
        for (auto const& item : json.items()) {
            const auto& key = item.key();
            const auto& val = item.value();

            auto fld_it = std::find_if(
                db_it->table.fields.begin(),
                db_it->table.fields.end(),
                [&](auto const& f) { return f.name == key; }
            );
            if (fld_it == db_it->table.fields.end())
                continue;  // это поле не относится к нашей схеме

            return 0;       // все ок, готовы к добавлению
        }
        return -1;
    }


public:
    databasePawnExtension() {
        clear_data_();
    }

    ~databasePawnExtension() {
        clear_data_();
    }

    int AddDataToPayload(int32_t UUID, int DB, njson json) {
        if (check_uuid_(UUID, DB) != 0) return -4;//0
        //WriteLock lock(data_mutex_);
        payload_[UUID].emplace_back(json);
        return 0;
    }

    int GetPayloadString(int32_t UUID, const std::string field, std::string& resp) {
        auto it = payload_.find(UUID);
        if (it == payload_.end()) return -1;

        for (const auto& obj : it->second) {
            if (!obj.contains(field)) continue;
            if (!obj[field].is_string()) return -3;
            resp = obj[field].get<std::string>();
            return 0;
        }
        return -2;
    }

    int GetPayloadInt(int32_t UUID, const std::string field, int& resp) {
        auto it = payload_.find(UUID);
        if (it == payload_.end()) return -1;

        for (const auto& obj : it->second) {
            if (!obj.contains(field)) continue;
            if (!obj[field].is_number_integer()) return -3;
            resp = obj[field].get<int>();
            return 0;
        }
        return -2;
    }

    int GetPayloadBool(int32_t UUID, const std::string field, bool& resp) {
        auto it = payload_.find(UUID);
        if (it == payload_.end()) return -1;

        for (const auto& obj : it->second) {
            if (!obj.contains(field)) continue;
            if (!obj[field].is_boolean()) return -3;
            resp = obj[field].get<bool>();
            return 0;
        }
        return -2;
    }

    int GetPayloadFloat(int32_t UUID, const std::string field, float& resp) {
        auto it = payload_.find(UUID);
        if (it == payload_.end()) return -1;

        for (const auto& obj : it->second) {
            if (!obj.contains(field)) continue;
            if (!obj[field].is_number_float() && !obj[field].is_number_integer()) return -3;
            resp = obj[field].get<float>();
            return 0;
        }
        return -2;
    }

    int GetIntFromJson(const int32_t& UUID, const int& doc_id, const std::string& field, int &result) {
        const njson resp = find_response_[UUID].back();
        if (checkFieldAvailability(resp, doc_id, field)) {
            const njson document = resp["documents"][doc_id];
            if (document[field].is_number_integer()) {
                //ReadLock lock(data_mutex_);
                result = document[field].get<int>();
                return 0; // all ok
            }
            else {
                return -2; // ожидается вызов другой функции для данного типа
            }
        }
        return -1; // поле не было найдено в документе
    }

    int GetFloatFromJson(const int32_t& UUID, const int& doc_id, const std::string& field, float& result) {
        const njson resp = find_response_[UUID].back();
        if (checkFieldAvailability(resp, doc_id, field)) {
            const njson document = resp["documents"][doc_id];
            if (document[field].is_number_float() || document[field].is_number_integer()) {
                //ReadLock lock(data_mutex_);
                result = document[field].get<float>();
                return 0; // all ok
            }
            else {
                return -2; // ожидается вызов другой функции для данного типа
            }
        }
        return -1; // поле не было найдено в документе
    }

    int GetStringFromJson(const int32_t& UUID, const int& doc_id, const std::string& field, std::string& result) {
        const njson resp = find_response_[UUID].back();
        if (checkFieldAvailability(resp, doc_id, field)) {
            const njson document = resp["documents"][doc_id];
            if (document[field].is_string()) {
                //ReadLock lock(data_mutex_);
                result = document[field].get<std::string>();
                return 0; // all ok
            }
            else {
                return -2; // ожидается вызов другой функции для данного типа
            }
        }
        return -1; // поле не было найдено в документе
    }

    int GetBoolFromJson(const int32_t& UUID, const int& doc_id, const std::string& field, bool& result) {
        const njson resp = find_response_[UUID].back();
        if (checkFieldAvailability(resp, doc_id, field)) {
            const njson document = resp["documents"][doc_id];
            if (document[field].is_boolean()) {
                //ReadLock lock(data_mutex_);
                result = document[field].get<bool>();
                return 0; // all ok
            }
            else {
                return -2; // ожидается вызов другой функции для данного типа
            }
        }
        return -1; // поле не было найдено в документе
    }

    void setPawn(IPawnComponent* pawn) {
        pawn_ = pawn;
    }

    void clearPawn() {
        pawn_ = nullptr;
    }

    int openSchema(std::string const& path) {
        std::vector<Schemas::DatabaseSchema> dbs = Schemas::parseSchemaFile(path);
        ++data_;
        storage_.insert({ data_, dbs });
        return data_;
    }

    int getCount() const {
        return data_;
    }

    int32_t getUUIDCall(const int& database) {
        static std::mt19937 rng(std::random_device{}());
        static std::uniform_int_distribution<int32_t> dist(1, std::numeric_limits<int32_t>::max());

        constexpr int max_attempts = 1000000;
        for (int i = 0; i < max_attempts; ++i) {
            //WriteLock lock(data_mutex_);
            int32_t candidate = dist(rng);
            if (UUID_Call_.find(candidate) == UUID_Call_.end()) {
                //WriteLock lock(data_mutex_);
                UUID_Call_[candidate] = database;
                return candidate;
            }
        }
        return -1;
    }

    void clearUUID(const int32_t& UUID) {
        //WriteLock lock(data_mutex_);

        UUID_Call_.erase(UUID);
        call_data_.erase(UUID);
        filter_data_.erase(UUID);
        projection_field_.erase(UUID);
        find_response_.erase(UUID);

        call_sharded_id_.erase(UUID);
        add_sharded_data_.erase(UUID);
        get_sharded_fields_.erase(UUID);
        find_sharded_data_.erase(UUID);
        update_sharded_data_.erase(UUID);
        find_get_sharded_response_.erase(UUID);
        add_ref_.erase(UUID);

        payload_.erase(UUID);
    }

    int appendProjectionData(int32_t uuid,
        int DB,
        const std::string& collection_name,
        const njson& json
    ) {
        if (check_uuid_(uuid, DB) != 0) return -4;//0

        const auto [result, db_it] = check_db_(DB, collection_name, Schemas::DBType::MONGO);
        if (result != 0) return -3;//1

        if(parse_json_scheme_(json, db_it) == 0)//2
        {
            projection_field_[uuid].emplace_back(json);
            return 0;
        }

        return -1;  // ни одно поле не подошло
    }

    int appendCallData(int32_t uuid,
        int DB,
        const std::string& collection_name,
        const njson& json
    ) {
        if (check_uuid_(uuid, DB) != 0) return -4;//0

        const auto [result, db_it] = check_db_(DB, collection_name, Schemas::DBType::MONGO);
        if (result != 0) return -3;//1

        if (parse_json_scheme_(json, db_it) == 0) {
            //WriteLock lock(data_mutex_);
            call_data_[uuid].emplace_back(json);
            return 0;       // успешно добавили
        }

        return -1;  // ни одно поле не подошло
    }

    int appendFilterData(int32_t uuid,
        int DB,
        const std::string& collection_name,
        const njson& json
    ) {
        if (check_uuid_(uuid, DB) != 0) return -4;//0

        const auto [result, db_it] = check_db_(DB, collection_name, Schemas::DBType::MONGO);
        if (result != 0) return -3;//1

        if (parse_json_scheme_(json, db_it) == 0) {
            //WriteLock lock(data_mutex_);
            filter_data_[uuid].emplace_back(json);
            return 0;       // успешно добавили
        }

        return -1;  // ни одно поле не подошло
    }

    // FIND
    // forward CALLBACK(UUID, bool:is_error, code, documents);
    void ExecuteFind(
        const int32_t UUID,
        const std::string collection,
        const std::string callback,
        const std::string sort_field = "",
        const int sort_order = 1,
        const int limit = 10,
        const int offset = 0
    ) {
        njson result_filter;
        for (const auto& obj : filter_data_[UUID]) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                result_filter[it.key()] = it.value(); // перезаписывает при совпадении ключей
            }
        }
        njson result_projection;
        for (const auto& obj : projection_field_[UUID]) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                result_projection[it.key()] = it.value(); // перезаписывает при совпадении ключей
            }
        }
        njson request = {
            {"collection", collection},
            {"filter", result_filter},
            {"projection", result_projection},
            {"sort_field", sort_field},
            {"sort_order", sort_order},
            {"limit", limit},
            {"offset", offset}
        };

        CurlClient client(url_host.c_str(), api_key_for_host.c_str());
        const std::string rq = request.dump();
        auto [response, code] = client.postJson("/find", rq);
        
        njson rsp = njson::parse(response);
        if (rsp.contains("documents") && rsp["documents"].is_array()) {
            find_response_[UUID].push_back(rsp);
        }

        if (code != 200) {
            for (IPawnScript* script : pawn_->sideScripts()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code, 0);
            }

            if (auto script = pawn_->mainScript()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code, 0);
            }
            return;
        }
        int count_docs = 0;

        if (rsp.contains("documents") && rsp["documents"].is_array()) {
            count_docs = (int)rsp["documents"].size();
        }

        for (IPawnScript* script : pawn_->sideScripts()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code, count_docs);
        }

        if (auto script = pawn_->mainScript()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code, count_docs);
        }
        return;
    }

    //UPDATE FORWARD
    //forward CALLBACK(UUID, bool:is_error, code);
    void ExecuteUpdate(const int32_t UUID, const std::string collection, const std::string callback) {
        njson result_filter;
        for (const auto& obj : filter_data_[UUID]) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                result_filter[it.key()] = it.value(); // перезаписывает при совпадении ключей
            }
        }
        njson result_data;
        for (const auto& obj : call_data_[UUID]) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                result_data[it.key()] = it.value(); // перезаписывает при совпадении ключей
            }
        }
        njson request = { 
            {"collection", collection},
            {"filter", result_filter},
            {"update", result_data}
        };

        CurlClient client(url_host.c_str(), api_key_for_host.c_str());
        const std::string rq = request.dump();
        auto [response, code] = client.postJson("/update", rq);

        if (code != 200) {
            for (IPawnScript* script : pawn_->sideScripts()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code);
            }

            if (auto script = pawn_->mainScript()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code);
            }
            return;
        }

        for (IPawnScript* script : pawn_->sideScripts()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code);
        }

        if (auto script = pawn_->mainScript()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code);
        }
    }

    //DELETE FORWARD
    //forward CALLBACK(UUID, bool:is_error, code);
    void ExecuteDelete(const int32_t UUID, const std::string collection, const std::string callback) {
        njson result;
        for (const auto& obj : filter_data_[UUID]) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                result[it.key()] = it.value(); // перезаписывает при совпадении ключей
            }
        }
        njson request = { 
            {"collection", collection},
            {"filter", result}
        };

        CurlClient client(url_host.c_str(), api_key_for_host.c_str());
        const std::string rq = request.dump();
        auto [response, code] = client.postJson("/delete", rq);

        if (code != 200) {
            for (IPawnScript* script : pawn_->sideScripts()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code);
            }

            if (auto script = pawn_->mainScript()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code);
            }
            return;
        }

        for (IPawnScript* script : pawn_->sideScripts()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code);
        }

        if (auto script = pawn_->mainScript()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code);
        }
    }

    //INSERT FORWARD
    // forward CALLBACK(UUID, bool:is_error, code);
    void ExecuteInsert(const int32_t UUID, const std::string collection, const std::string callback) {
        njson result;
        //ReadLock lock(data_mutex_);
        for (const auto& obj : call_data_[UUID]) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                result[it.key()] = it.value(); // перезаписывает при совпадении ключей
            }
        }

        njson request = { {"collection", collection},
                          {"document", result}
                        };

        CurlClient client(url_host.c_str(), api_key_for_host.c_str());
        const std::string rq = request.dump();
        auto [response, code] = client.postJson("/insert", rq);

        if (code != 200) {
            for (IPawnScript* script : pawn_->sideScripts()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code);
            }

            if (auto script = pawn_->mainScript()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code);
            }
            return;
        }
        
        for (IPawnScript* script : pawn_->sideScripts()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code);
        }

        if (auto script = pawn_->mainScript()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code);
        }
    }

    //////////////// SHARDED

    int GetStringSharded(int32_t UUID, int doc_id, const std::string field, std::string& response) {
        if (find_get_sharded_response_[UUID].is_array()) {
            response = find_get_sharded_response_[UUID][doc_id][field].get<std::string>();
            return 0;
        }
        return -1;
    }

    int SetRecordForShardedOperation(int32_t UUID, int DB, std::string collection_name, std::string record) {
        if (check_uuid_(UUID, DB) != 0) return -4;//0

        const auto [result, db_it] = check_db_(DB, collection_name, Schemas::DBType::SHARDED);
        if (result != 0) return -3;//1

        call_sharded_id_[UUID] = record;

        return 0;
    }

    int AddFieldSharded(int32_t UUID, int DB, std::string collection_name, njson json) {
        if (check_uuid_(UUID, DB) != 0) return -4;//0

        const auto [result, db_it] = check_db_(DB, collection_name, Schemas::DBType::SHARDED);
        if (result != 0) return -3;//1

        // 2) Перебирать только ключи JSON-а, искать поле и проверять тип
        if (parse_json_scheme_(json, db_it) == 0) {
            if (json.is_object() && !json.empty()) {
                //WriteLock lock(data_mutex_);
                get_sharded_fields_[UUID].push_back(json.begin().key());
            }
            return 0;       // успешно добавили
        }
        return 0;
    }
    // post /records (add)
    /*
        CurlClient client("http://127.0.0.1:8000", "", "admin", "password");

        std::string json = R"({
            "username": "john",
            "password_hash": "abc123",
            "ip_reg": "192.168.0.1",
            "last_logged": "2025-07-01T00:00:00",
            "last_ip": "10.0.0.1"
        })";

        auto [response, status] = client.postJson("/records", json);
        std::cout << "POST /records → " << status << "\n" << response << "\n";
    */
    int AddStringToInsertToSharded(int32_t UUID, int DB, std::string collection_name, njson json) {
        if (check_uuid_(UUID, DB) != 0) return -4;//0

        const auto [result, db_it] = check_db_(DB, collection_name, Schemas::DBType::SHARDED);
        if (result != 0) return -3;//1

        // 2) Перебирать только ключи JSON-а, искать поле и проверять тип
        if (parse_json_scheme_(json, db_it) == 0) {
            //WriteLock lock(data_mutex_);
            add_sharded_data_[UUID].emplace_back(json);
            return 0;       // успешно добавили
        }

        return -1;
    }

    // forward CALLBACK(UUID, bool:error, code, const string[]);
    void ExecuteAddSharded(const int32_t UUID, const std::string collection, const std::string callback) {
        njson result;
        for (const auto& obj : add_sharded_data_[UUID]) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                result[it.key()] = it.value(); // перезаписывает при совпадении ключей
            }
        }

        CurlClient client(sharded_url_host.c_str(), "", sharded_login.c_str(), sharded_pass.c_str());
        const std::string rq = result.dump();
        auto [response, code] = client.postJson("/records", rq);

        if (code != 200) {
            for (IPawnScript* script : pawn_->sideScripts()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, (int)code);
            }

            if (auto script = pawn_->mainScript()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, (int)code);
            }
            return;
        }

        try {
            const njson rjs = njson::parse(response);
            if (rjs.contains("ref") && rjs["ref"].is_string()) {
                add_ref_[UUID] = rjs["ref"].get<std::string>();
            }
        }
        catch (const std::exception& e) {
            add_ref_[UUID] = "";
            //logging
        }

        for (IPawnScript* script : pawn_->sideScripts()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, (int)code);//std::string(njson::parse(response)["ref"]));
        }

        if (auto script = pawn_->mainScript()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, (int)code);//std::string(njson::parse(response)["ref"]));
        }
    }
    bool GetRefFromAdd(int32_t UUID, std::string& refferer) {
        auto it = add_ref_.find(UUID);
        if (it == add_ref_.end()) {
            refferer = "_";
            return false;
        }
        refferer = add_ref_[UUID];
        return true;
    }

    // get /records/* (get)
    /*
        auto [response, status] = client.get("/records/a0:3?fields=username,last_ip");
        std::cout << "GET /records/a0:3 → " << status << "\n" << response << "\n";
    */
    //forward CALLBACK(UUID, bool:error, code, doc_count);
    void ExecuteGetSharded(const int32_t UUID, const std::string collection, const std::string callback) {
        std::string path = "/records/" + call_sharded_id_[UUID] + "?fields=";

        bool start = false;
        for (const auto& obj : get_sharded_fields_[UUID]) {
            if (!start)
                path = path + obj;
            else path = path + "," + obj;
            start = true;
        }

        CurlClient client(sharded_url_host.c_str(), "", sharded_login.c_str(), sharded_pass.c_str());
        auto [response, code] = client.get(path);

        if (code != 200) {
            for (IPawnScript* script : pawn_->sideScripts()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code, 0);
            }

            if (auto script = pawn_->mainScript()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code, 0);
            }
            return;
        }

        nlohmann::json j = nlohmann::json::array();
        j.push_back( njson::parse(response) );

        find_get_sharded_response_[UUID] = j;

        for (IPawnScript* script : pawn_->sideScripts()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code, 1);
        }

        if (auto script = pawn_->mainScript()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code, 1);
        }
    }

    // put /records/* (update)
    /*
        std::string formBody = "username=johnny&last_ip=127.0.0.2";
        auto [response, status] = client.putForm("/records/a0:3", formBody);
        std::cout << "PUT /records/a0:3 → " << status << "\n" << response << "\n";
    */
    int AddUpdateDataSharded(int32_t UUID, int DB, std::string collection_name, njson json) {
        if (check_uuid_(UUID, DB) != 0) return -4;//0

        const auto [result, db_it] = check_db_(DB, collection_name, Schemas::DBType::SHARDED);
        if (result != 0) return -3;//1

        // 2) Перебирать только ключи JSON-а, искать поле и проверять тип
        if (parse_json_scheme_(json, db_it) == 0) {
            //WriteLock lock(data_mutex_);
            update_sharded_data_[UUID].emplace_back(json);
            return 0;       // успешно добавили
        }
        return 0;
    }

    void ExecuteUpdateSharded(const int32_t UUID, const std::string collection, const std::string callback) {
        njson result;
        for (const auto& obj : update_sharded_data_[UUID]) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                result[it.key()] = it.value(); // перезаписывает при совпадении ключей
            }
        }

        bool first = true;
        std::string body;
        for (auto it = result.begin(); it != result.end(); ++it)
        {
            if (!first) 
                body += '&';

            first = false;

            body += it.key();
            body += '=';
            body += it.value();
        }

        const std::string url_path = "/records/" + call_sharded_id_[UUID]+"?"+body;

        CurlClient client(sharded_url_host.c_str(), "", sharded_login.c_str(), sharded_pass.c_str());
        auto [response, code] = client.putForm(url_path.c_str(), body);

        if (code != 200) {
            for (IPawnScript* script : pawn_->sideScripts()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code);
            }

            if (auto script = pawn_->mainScript()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code);
            }
            return;
        }

        for (IPawnScript* script : pawn_->sideScripts()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code);
        }

        if (auto script = pawn_->mainScript()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code);
        }
    }

    // delete /records/* (delete)
    /*
        auto [response, status] = client.del("/records/a0:3");
        std::cout << "DELETE /records/a0:3 → " << status << "\n" << response << "\n";
    */
    void ExecuteDeleteSharded(const int32_t UUID, const std::string collection, const std::string callback) {
        const std::string url_path = "/records/" + call_sharded_id_[UUID];
        CurlClient client(sharded_url_host.c_str(), "", sharded_login.c_str(), sharded_pass.c_str());
        auto [response, code] = client.del(url_path);

        if (code != 200) {
            for (IPawnScript* script : pawn_->sideScripts()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code);
            }

            if (auto script = pawn_->mainScript()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code);
            }
            return;
        }

        for (IPawnScript* script : pawn_->sideScripts()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code);
        }

        if (auto script = pawn_->mainScript()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code);
        }
    }

    // get /find?* (search)
    /*
        auto [response, status] = client.get("/find?field=username&value=john&fields=last_ip,last_logged");
        std::cout << "GET /find → " << status << "\n" << response << "\n";
    */
    int AddFindDataSharded(int32_t UUID, int DB, std::string collection_name, njson json) {
        if (check_uuid_(UUID, DB) != 0) return -4;//0

        const auto [result, db_it] = check_db_(DB, collection_name, Schemas::DBType::SHARDED);
        if (result != 0) return -3;//1

        // 2) Перебирать только ключи JSON-а, искать поле и проверять тип
        if (parse_json_scheme_(json, db_it) == 0) {
            //WriteLock lock(data_mutex_);
            find_sharded_data_[UUID] = json;
            return 0;       // успешно добавили
        }
        return -1;
    }

    void ExecuteFindSharded(const int32_t UUID, const std::string collection, const std::string callback) {
        const std::string field = find_sharded_data_[UUID].begin().key();
        const std::string value = std::string(find_sharded_data_[UUID].begin().value());
        std::string path = "/find?field=" + field + "&value=" + value+"&fields=";

        njson result;
        for (const auto& obj : update_sharded_data_[UUID]) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                result[it.key()] = it.value(); // перезаписывает при совпадении ключей
            }
        }

        bool first = false;
        for (auto it = result.begin(); it != result.end(); ++it) {
            if (first)
                path += ",";

            path = path + it.key();
            first = true;
        }

        CurlClient client(sharded_url_host.c_str(), "", sharded_login.c_str(), sharded_pass.c_str());
        auto [response, code] = client.get(path);

        if (code != 200) {
            for (IPawnScript* script : pawn_->sideScripts()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code, 0);
            }

            if (auto script = pawn_->mainScript()) {
                script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, true, code, 0);
            }
            return;
        }

        find_get_sharded_response_[UUID] = njson::parse(response);

        for (IPawnScript* script : pawn_->sideScripts()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code, (int)njson::parse(response).size());
        }

        if (auto script = pawn_->mainScript()) {
            script->Call(callback.c_str(), DefaultReturnValue_True, (int)UUID, false, code, (int)njson::parse(response).size());
        }
    }

};

class databasePlugin : public IComponent, public PawnEventHandler {
private:
    ICore* core_ = nullptr;
    IPawnComponent* pawn_ = nullptr;
public:
    PROVIDE_UID(0x4B516D83FCCEAEA3);

    ~databasePlugin() {
        if (pawn_) {
            pawn_->getEventDispatcher().removeEventHandler(this);
        }
        if (core_) {

        }
    }

    // Implement the pawn script listener API.
	void onAmxLoad(IPawnScript& script) override
	{
		// Because we're using `SCRIPT_API` this call automatically registers the declared natives.
		pawn_natives::AmxLoad(script.GetAMX());
	}

	void onAmxUnload(IPawnScript& script) override
	{

	}

	// Implement the main component API.
	StringView componentName() const override
	{
		return "Database controller";
	}

	SemanticVersion componentVersion() const override
	{
		return SemanticVersion(0, 0, 1, 0);
	}

	void onLoad(ICore* c) override
	{
		// Cache core, listen to player events.
		core_ = c;
        LoadConfig();
		core_->printLn("Database controller loaded.");
		setAmxLookups(core_);
	}

	void onInit(IComponentList* components) override
	{
		// Cache components, add event handlers here.
		pawn_ = components->queryComponent<IPawnComponent>();

		if (pawn_)
		{
			// For the legacy `amx_` C API this call sets the correct pointers so that pawn
			// function calls call the original versions within the server.
			setAmxFunctions(pawn_->getAmxFunctions());
			// For the pawn-natives system this call sets the various component references used for
			// parameter value lookups.
			setAmxLookups(components);
			// Register this component as wanting to be informed when a script is loaded.
			pawn_->getEventDispatcher().addEventHandler(this);

            databasePawnExtension::Get()->setPawn(pawn_);
		}
	}

	void onReady() override
	{
		// Fire events here at earliest.
	}

	void onFree(IComponent* component) override
	{
		// Invalidate pawn pointer so it can't be used past this point.
		if (component == pawn_)
		{
			pawn_ = nullptr;
            databasePawnExtension::Get()->clearPawn();
			setAmxFunctions();
			setAmxLookups();
		}
	}

	void free() override
	{
		// Deletes the component.
		delete this;
	}

	void reset() override
	{
		// Resets data when the mode changes.
	}
};

COMPONENT_ENTRY_POINT()
{
	return new databasePlugin();
}

// Открытие схемы
// native LoadScheme(const path[]);
SCRIPT_API(LoadScheme, int(std::string const& path)) {
    
    if (int result = databasePawnExtension::Get()->openSchema(path)) {
        return result;
    }
	return 0;
}

// Запросы к схемам
// native GetUUIDForCall(database);
SCRIPT_API(GetUUIDForCall, int(int database)) {
    int32_t result = databasePawnExtension::Get()->getUUIDCall(database);
    if (result >= 0) {
        return result;
    }
    return -1;
}

// native ClearUUID(UUID);
SCRIPT_API(ClearUUID, int(int UUID)) {
    databasePawnExtension::Get()->clearUUID(UUID);
    return 1;
}

// INSERT, DELETE
// native AddInRequestInt(UUID, DB, const collection_name[], const field[], data);
SCRIPT_API(AddInRequestInt, int(int UUID, int DB, const std::string& collection_name, const std::string& field, int data)) {
    const njson s = { {field, data} };
    return databasePawnExtension::Get()->appendCallData(UUID, DB, collection_name, s);
}

// native AddInRequestFloat(UUID, DB, const collection_name[], const field[], Float:data);
SCRIPT_API(AddInRequestFloat, int(int UUID, int DB, const std::string& collection_name, const std::string& field, float data)) {
    const njson s = { {field, data} };
    return databasePawnExtension::Get()->appendCallData(UUID, DB, collection_name, s);
}

// native AddInRequestString(UUID, DB, const collection_name[], const field[], const data[]);
SCRIPT_API(AddInRequestString, int(int UUID, int DB, const std::string& collection_name, const std::string& field, const std::string& data)) {
    const njson s = { {field, data} };
    return databasePawnExtension::Get()->appendCallData(UUID, DB, collection_name, s);
}

// native AddInRequestBool(UUID, DB, const collection_name[], const field[], bool:data);
SCRIPT_API(AddInRequestBool, int(int UUID, int DB, const std::string& collection_name, const std::string& field, bool data)) {
    const njson s = { {field, data} };
    return databasePawnExtension::Get()->appendCallData(UUID, DB, collection_name, s);
}

// INSERT
// native CallRequestInsert(UUID, const collection[], const callback[]);
SCRIPT_API(CallRequestInsert, bool(int UUID, const std::string& collection, const std::string& callback)) {
    //ThreadControl::CreateThreadEx(
        //[](int UUID, const std::string& collection, const std::string& callback) {
            databasePawnExtension::Get()->ExecuteInsert(UUID, collection, callback);
        //},
        //UUID, collection, callback
    //);
    return true;
}

// DELETE
// native CallRequestDelete(UUID, const collection[], const callback[]);
SCRIPT_API(CallRequestDelete, bool(int UUID, const std::string& collection, const std::string& callback)) {
    //ThreadControl::CreateThreadEx(
        //[](int UUID, const std::string& collection, const std::string& callback) {
            databasePawnExtension::Get()->ExecuteDelete(UUID, collection, callback);
        //},
        //UUID, collection, callback
    //);
    return true;
}

// UPDATE
// native AddInFilterInt(UUID, DB, const collection_name[], const field[], data);
SCRIPT_API(AddInFilterInt, int(int UUID, int DB, const std::string& collection_name, const std::string& field, int data)) { // https://music.yandex.ru/album/7164270/track/51390078
    const njson s = { {field, data} };
    return databasePawnExtension::Get()->appendFilterData(UUID, DB, collection_name, s);
}

// native AddInFilterFloat(UUID, DB, const collection_name[], const field[], Float:data);
SCRIPT_API(AddInFilterFloat, int(int UUID, int DB, const std::string& collection_name, const std::string& field, float data)) {
    const njson s = { {field, data} };
    return databasePawnExtension::Get()->appendFilterData(UUID, DB, collection_name, s);
}

// native AddInFilterString(UUID, DB, const collection_name[], const field[], const data[]);
SCRIPT_API(AddInFilterString, int(int UUID, int DB, const std::string& collection_name, const std::string& field, const std::string& data)) {
    const njson s = { {field, data} };
    return databasePawnExtension::Get()->appendFilterData(UUID, DB, collection_name, s);
}

// native AddInFilterBool(UUID, DB, const collection_name[], const field[], bool:data);
SCRIPT_API(AddInFilterBool, int(int UUID, int DB, const std::string& collection_name, const std::string& field, bool data)) {
    const njson s = { {field, data} };
    return databasePawnExtension::Get()->appendFilterData(UUID, DB, collection_name, s);
}

// native CallRequestUpdate(UUID, const collection[], const callback[]);
SCRIPT_API(CallRequestUpdate, bool(int UUID, const std::string& collection, const std::string& callback)) {
    //ThreadControl::CreateThreadEx(
        //[](int UUID, const std::string& collection, const std::string& callback) {
            databasePawnExtension::Get()->ExecuteUpdate(UUID, collection, callback);
        //},
        //UUID, collection, callback
    //);
    return true;
}

// FIND
// native AddProjectionField(UUID, DB, const collection_name[], const field[], status);
SCRIPT_API(AddProjectionField, int(int UUID, int DB, const std::string& collection_name, const std::string& field, int status)) {
    njson s = { {field, status} };
    return databasePawnExtension::Get()->appendProjectionData(UUID, DB, collection_name, s);
}

// native CallRequestFind(UUID, const collection[], const callback[], const sort_field[] = "", sort_order = 1, limit = 10, offset = 0);
SCRIPT_API(CallRequestFind, bool(int UUID, const std::string& collection, const std::string& callback, const std::string& sort_field, int sort_order, int limit, int offset)) {
    //ThreadControl::CreateThreadEx(
        //[](int UUID, const std::string& collection, const std::string& callback, const std::string& sort_field, int sort_order, int limit, int offset) {
            databasePawnExtension::Get()->ExecuteFind(UUID, collection, callback, sort_field, sort_order, limit, offset);
        //},
        //UUID, collection, callback, sort_field, sort_order, limit, offset
    //);
    return true;
}


// PARSING FIND
// https://music.yandex.ru/album/15814421/track/83836715
// native GetIntFromDocument(UUID, doc_id, const field[], &response);
SCRIPT_API(GetIntFromDocument, int(int UUID, int doc_id, const std::string& field, int& response)) {
    return databasePawnExtension::Get()->GetIntFromJson(UUID, doc_id, field, response);
}
// native GetFloatFromDocument(UUID, doc_id, const field[], &Float:response);
SCRIPT_API(GetFloatFromDocument, int(int UUID, int doc_id, const std::string& field, float& response)) {
    return databasePawnExtension::Get()->GetFloatFromJson(UUID, doc_id, field, response);
}

// native GetStringFromDocument(UUID, doc_id, const field[], response[], size = sizeof(response));
SCRIPT_API(GetStringFromDocument, int(int UUID, int doc_id, const std::string& field, OutputOnlyString& response)) {
    std::string resp_one;
    int resp_two = databasePawnExtension::Get()->GetStringFromJson(UUID, doc_id, field, resp_one);
    response = resp_one;
    return resp_two;
}

// native GetBoolFromDocument(UUID, doc_id, const field[], &bool:response);
SCRIPT_API(GetBoolFromDocument, int(int UUID, int doc_id, const std::string& field, bool& response)) {
    return databasePawnExtension::Get()->GetBoolFromJson(UUID, doc_id, field, response);
}

//////////// SHARDED

//native CreateRecordSharded(UUID, const collection[], const callback[]);
SCRIPT_API(CreateRecordSharded, bool(int UUID, const std::string& collection, const std::string& callback)) {
    //ThreadControl::CreateThreadEx(
        //[](int UUID, const std::string& collection, const std::string& callback) {
            databasePawnExtension::Get()->ExecuteAddSharded(UUID, collection, callback);
        //},
        //UUID, collection, callback
    //);
    return true;
}

//native UpdateRecordSharded(UUID, const collection[], const callback[]);
SCRIPT_API(UpdateRecordSharded, bool(int UUID, const std::string& collection, const std::string& callback)) {
    //ThreadControl::CreateThreadEx(
        //[](int UUID, const std::string& collection, const std::string& callback) {
            databasePawnExtension::Get()->ExecuteUpdateSharded(UUID, collection, callback);
        //},
        //UUID, collection, callback
    //);
    return true;
}

//native DeleteRecordSharded(UUID, const collecton[], const callback[]);
SCRIPT_API(DeleteRecordSharded, bool(int UUID, const std::string& collection, const std::string& callback)) {
    //ThreadControl::CreateThreadEx(
        //[](int UUID, const std::string& collection, const std::string& callback) {
            databasePawnExtension::Get()->ExecuteDeleteSharded(UUID, collection, callback);
        //},
        //UUID, collection, callback
    //);
    return true;
}

//native FindRecordSharded(UUID, const collection[], const callback[]);
SCRIPT_API(FindRecordSharded, bool(int UUID, const std::string& collection, const std::string& callback)) {
    //ThreadControl::CreateThreadEx(
        //[](int UUID, const std::string& collection, const std::string& callback) {
            databasePawnExtension::Get()->ExecuteFindSharded(UUID, collection, callback);
        //},
        //UUID, collection, callback
    //);
    return true;
}

//native GetRecordSharded(UUID, const collection[], const callback[]);
SCRIPT_API(GetRecordSharded, bool(int UUID, const std::string& collection, const std::string& callback)) {
    //ThreadControl::CreateThreadEx(
        //[](int UUID, const std::string& collection, const std::string& callback) {
            databasePawnExtension::Get()->ExecuteGetSharded(UUID, collection, callback);
        //},
        //UUID, collection, callback
    //);
    return true;
}

//native SetRecordShardedOperation(UUID, DB, const collection_name[], const record[]);
SCRIPT_API(SetRecordShardedOperation, int(int UUID, int DB, const std::string& collection, const std::string& record)) {
    return databasePawnExtension::Get()->SetRecordForShardedOperation(UUID, DB, collection, record);
}

//native AddFieldSharded(UUID, DB, const collection_name[], const field[]);
SCRIPT_API(AddFieldSharded, int(int UUID, int DB, const std::string& collection, const std::string& field)) {
    const njson s = { {field, 1} };
    return databasePawnExtension::Get()->AddFieldSharded(UUID, DB, collection, s);
}

//native AddStringInsertSharded(UUID, DB, const collection_name[], const field[], const string[]);
SCRIPT_API(AddStringInsertSharded, int(int UUID, int DB, const std::string& collection, const std::string& field, const std::string& str)) {
    const njson s = { {field,str} };
    return databasePawnExtension::Get()->AddStringToInsertToSharded(UUID, DB, collection, s);
}

//native AddUpdateDataSharded(UUID, DB, const collection_name[], const field[], const string[]);
SCRIPT_API(AddUpdateDataSharded, int(int UUID, int DB, const std::string& collection, const std::string& field, const std::string& str)) {
    const njson s = { {field,str} };
    return databasePawnExtension::Get()->AddUpdateDataSharded(UUID, DB, collection, s);
}

//native AddFindDataSharded(UUID, DB, const collection_name[], const field[], const string[]);
SCRIPT_API(AddFindDataSharded, int(int UUID, int DB, const std::string& collection, const std::string& field, const std::string& str)) {
    const njson s = { {field,str} };
    return databasePawnExtension::Get()->AddFindDataSharded(UUID, DB, collection, s);
}

//native GetStringSharded(UUID, doc_id, const field[], &output[], size = sizeof(output));
SCRIPT_API(GetStringSharded, int(int UUID, int doc_id, const std::string& field, OutputOnlyString& response)) {
    std::string resp;
    const int result = databasePawnExtension::Get()->GetStringSharded(UUID, doc_id, field, resp);
    if (result == 0) 
        response = resp;
    else
        response = std::string("");

    return result;
}

// PAYLOAD
// https://music.yandex.ru/album/36530037/track/138936959
//native AddPayloadString(UUID, DB, const field[], const string[]);
SCRIPT_API(AddPayloadString, int(int UUID, int DB, const std::string& field, const std::string& str)) {
    const njson s = { {field, str} };
    return databasePawnExtension::Get()->AddDataToPayload(UUID, DB, s);
}

//native AddPayloadInt(UUID, DB, const field[], int);
SCRIPT_API(AddPayloadInt, int(int UUID, int DB, const std::string& field, int num)) {
    const njson s = { {field, num} };
    return databasePawnExtension::Get()->AddDataToPayload(UUID, DB, s);
}

//native AddPayloadBool(UUID, DB, const field, bool:state);
SCRIPT_API(AddPayloadBool, int(int UUID, int DB, const std::string& field, bool state)) {
    const njson s = { {field, state} };
    return databasePawnExtension::Get()->AddDataToPayload(UUID, DB, s);
}

//native AddPayloadFloat(UUID, DB, const field, Float:flt);
SCRIPT_API(AddPayloadFloat, int(int UUID, int DB, const std::string& field, float flt)) {
    const njson s = { {field, flt} };
    return databasePawnExtension::Get()->AddDataToPayload(UUID, DB, s);
}

//native GetPayloadString(UUID, const field[], string[]);
SCRIPT_API(GetPayloadString, int(int UUID, const std::string& field, OutputOnlyString& response)) {
    std::string resp;
    const int result = databasePawnExtension::Get()->GetPayloadString(UUID, field, resp);
    if (result == 0)
        response = resp;
    else
        response = std::string("");

    return result;
}

//native GetPayloadInt(UUID, const field[], &int);
SCRIPT_API(GetPayloadInt, int(int UUID, const std::string& field, int& response)) {
    int resp;
    const int result = databasePawnExtension::Get()->GetPayloadInt(UUID, field, resp);
    if (result == 0)
        response = resp;
    else
        response = 666;
    return result;
}

//native GetPayloadBool(UUID, const field[], &bool:state);
SCRIPT_API(GetPayloadBool, int(int UUID, const std::string& field, bool& response)){
    bool resp;
    const int result = databasePawnExtension::Get()->GetPayloadBool(UUID, field, resp);
    if (result == 0)
        response = resp;
    else
        response = false;
    return result;
}

//native GetPayloadFloat(UUID, const field[], &Float:flt);
SCRIPT_API(GetPayloadFloat, int(int UUID, const std::string& field, float& response)) {
    float resp;
    const int result = databasePawnExtension::Get()->GetPayloadFloat(UUID, field, resp);
    if (result == 0)
        response = resp;
    else
        response = 666.f;
    return result;
}

//native GetRefFromAdd(UUID, str[], size = sizeof(output));
SCRIPT_API(GetRefFromAdd, bool(int UUID, OutputOnlyString& str)) {
    std::string s;
    const bool result = databasePawnExtension::Get()->GetRefFromAdd(UUID, s);
    str = s;
    return result;
}
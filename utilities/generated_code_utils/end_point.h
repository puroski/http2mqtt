//
// Created by nomenas on 23-03-20.
//

#ifndef HTTP2MQTTBRIDGE_MQTT_RESOURCE_H
#define HTTP2MQTTBRIDGE_MQTT_RESOURCE_H

#include <mqtt_client.h>

class EndPoint {
public:
  using PostItemCallback = std::function<void(CreateItemStatus status_code, const boost::uuids::uuid&)>;
  using PostItemHandler = std::function<void(const tao::json::value& body, PostItemCallback callback)>;
  using GetItemCallback = std::function<void(GetItemStatus status_code, const tao::json::value& object)>;
  using GetItemHandler = std::function<void(const boost::uuids::uuid&, GetItemCallback)>;
  using GetItemsCallback = std::function<void(GetItemStatus status_code, const tao::json::value& object)>;
  using GetItemsHandler = std::function<void(const boost::uuids::uuid& cursor,
      const std::vector<boost::uuids::uuid>& ids, GetItemsCallback callback)>;
  using PutItemCallback = std::function<void(UpdateItemStatus status_code)>;
  using PutItemHandler = std::function<void(const boost::uuids::uuid&, const tao::json::value& body, PutItemCallback callback)>;
  using DeleteItemCallback = std::function<void(DeleteItemStatus status_code)>;
  using DeleteItemHandler = std::function<void(const boost::uuids::uuid&, DeleteItemCallback callback)>;

  EndPoint(std::string resource_name, std::string root_topic, MqttClient& mqtt_client)
      : mqtt_client_{mqtt_client}
      , root_topic_{std::move(root_topic)}
      , resource_name_(std::move(resource_name)) {}

  void RegisterPostItemHandler(PostItemHandler handler) {
    mqtt_client_.Subscribe(root_topic_ + "/api/" + resource_name_ + "/POST",
        std::bind(&EndPoint::OnPostMessage, this, std::placeholders::_2, std::move(handler)));
  }

  void RegisterGetItemHandler(GetItemHandler handler) {
    mqtt_client_.Subscribe(root_topic_ + "/api/" + resource_name_ + "/+/GET",
        std::bind(&EndPoint::OnGetItemMessage, this, std::placeholders::_1, std::placeholders::_2, std::move(handler)));
  }

  void RegisterGetItemsHandler(GetItemsHandler handler) {
    mqtt_client_.Subscribe(root_topic_ + "/api/" + resource_name_ + "/GET",
        std::bind(&EndPoint::OnGetItemsMessage, this, std::placeholders::_2, std::move(handler)));
  }

  void RegisterPutItemHandler(PutItemHandler handler) {
    mqtt_client_.Subscribe(root_topic_ + "/api/" + resource_name_ + "/+/PUT",
        std::bind(&EndPoint::OnPutMessage, this, std::placeholders::_1, std::placeholders::_2, std::move(handler)));
  }

  void RegisterDeleteItemHandler(DeleteItemHandler handler) {
    mqtt_client_.Subscribe(root_topic_ + "/api/" + resource_name_ + "/+/DELETE",
        std::bind(&EndPoint::OnDeleteMessage, this, std::placeholders::_1, std::placeholders::_2, std::move(handler)));
  }

  template <typename T>
  void PublishEvent(const Event<User>& event) {
    const auto object = Serialize<T>(event.object);
    ThrowIfNotValid<T, ActionType::Get>(object);

    tao::json::value event_json = {
        {"id", boost::lexical_cast<std::string>(event.id)}
    };
    switch (event.action_type) {
    case ActionType::Create:
      event_json["event_type"] = "Created";
      event_json["payload"] = tao::json::to_string(object);
      break;
    case ActionType::Update:
      event_json["event_type"] = "Updated";
      event_json["payload"] = tao::json::to_string(object);
      break;
    case ActionType::Delete:
      event_json["event_type"] = "Deleted";
      break;
    }

    mqtt_client_.Publish(root_topic_ + "/event/" + resource_name_, tao::json::to_string(event_json));
  }

  template <typename T>
  static std::function<void(GetItemStatus status_code, const std::vector<std::reference_wrapper<const T>>& items)> CreateGetItemsWrapperCallback(GetItemsCallback callback) {
    return [callback = std::move(callback)](GetItemStatus status_code, const std::vector<std::reference_wrapper<const T>>& items) {
      tao::json::value object = tao::json::empty_array;

      try {
        std::vector< tao::json::value> array;
        for (auto&& item : items) {
          array.emplace_back(Serialize<T>(item));
          ThrowIfNotValid<T, ActionType::Get>(array.back());
        }
        object = array;
      } catch (const std::exception&) {
        status_code = GetItemStatus::InternalError;
        object = tao::json::empty_array;
      }

      callback(status_code, object);
    };
  }

  template <typename T>
  static std::function<void(GetItemStatus status_code, boost::optional<const T&> item)> CreateGetItemWrapperCallback(GetItemCallback callback) {
    return [callback = std::move(callback)](GetItemStatus status_code, boost::optional<const T&> item) {
      tao::json::value object = tao::json::null;

      try {
        object = Serialize<T>(item.value());
        ThrowIfNotValid<T, ActionType::Get>(object);
      } catch (const std::exception&) {
        status_code = item ? GetItemStatus::InternalError : GetItemStatus::ItemDoesntExist;
        object = tao::json::null;
      }

      callback(status_code, object);
    };
  }

protected:
  void OnPostMessage(const std::string& message, PostItemHandler handler) {
    size_t session_id = {};
    try {
      const tao::json::value request = tao::json::from_string(message);
      const auto body = request.at("body");
      session_id = request.as<size_t>("session_id");
      tao::json::value response = {{"session_id", session_id}};
      handler(body, GetPostItemCallback(std::move(response)));
    } catch (const std::exception& ex) {
      tao::json::value response = {{"session_id", session_id}};
      response["status"] = 400;
      response["body"] = std::string("{\"error\": \"") + ex.what() + "\"}";
      mqtt_client_.Publish(root_topic_ + "/response", tao::json::to_string(response));
    }
  }

  void OnGetItemMessage(const WildcardValue& wildcard_value, const std::string& message, GetItemHandler handler) {
    size_t session_id = {};
    try {
      const tao::json::value request = tao::json::from_string(message);
      session_id = request.as<size_t>("session_id");
      tao::json::value response = {{"session_id", session_id}};
      handler(boost::lexical_cast<boost::uuids::uuid>(wildcard_value), GetGetItemCallback(std::move(response)));
    } catch (const std::exception& ex) {
      tao::json::value response = {{"session_id", session_id}};
      response["status"] = 404;
      response["body"] = "{\"error\": \"Resource doesn't exist\"}";
      mqtt_client_.Publish(root_topic_ + "/response", tao::json::to_string(response));
    }
  }

  void OnGetItemsMessage(const std::string& message, GetItemsHandler handler) {
    size_t session_id = {};
    try {
      const tao::json::value request = tao::json::from_string(message);
      session_id = request.as<size_t>("session_id");
      tao::json::value response = {{"session_id", session_id}};
      boost::uuids::uuid cursor = {}; // read from the message
      std::vector<boost::uuids::uuid> ids = {}; // read from the message
      handler(cursor, ids, GetGetItemsCallback(std::move(response)));
    } catch (const std::exception& ex) {
      tao::json::value response = {{"session_id", session_id}};
      response["status"] = 404;
      response["body"] = "{\"error\": \"Resource doesn't exist\"}";
      mqtt_client_.Publish(root_topic_ + "/response", tao::json::to_string(response));
    }
  }

  void OnPutMessage(const WildcardValue& wildcard_value, const std::string& message, PutItemHandler handler) {
    size_t session_id = {};
    try {
      const tao::json::value request = tao::json::from_string(message);
      const auto body = request.at("body");
      session_id = request.as<size_t>("session_id");
      tao::json::value response = {{"session_id", session_id}};
      handler(boost::lexical_cast<boost::uuids::uuid>(wildcard_value), body, GetPutItemCallback(std::move(response)));
    } catch (const std::exception& ex) {
      tao::json::value response = {{"session_id", session_id}};
      response["status"] = 400;
      response["body"] = std::string("{\"error\": \"") + ex.what() + "\"}";
      mqtt_client_.Publish(root_topic_ + "/response", tao::json::to_string(response));
    }
  }

  PostItemCallback GetPostItemCallback(tao::json::value response) {
    return [mqtt_client = mqtt_client_, root_topic = root_topic_, response = std::move(response)](
               CreateItemStatus status_code, const boost::uuids::uuid &uuid) mutable {
      switch (status_code) {
      case CreateItemStatus::Success:
        response["status"] = 200;
        response["body"] =
            "{\"uuid\": \"" + boost::lexical_cast<std::string>(uuid) + "\"}";
        break;
      case CreateItemStatus::ItemAlreadyExist:
        response["status"] = 400;
        response["body"] = "{\"error\": \"Item already exist\"}";
        break;
      case CreateItemStatus::OperationForbidden:
        response["status"] = 403;
        response["body"] = "{\"error\": \"Operation forbidden\"}";
        break;
      case CreateItemStatus::OutOfMemory:
        response["status"] = 507;
        response["body"] = "{\"error\": \"Out of memory\"}";
        break;
      }

      mqtt_client.Publish(root_topic + "/response", tao::json::to_string(response));
    };
  }

  void OnDeleteMessage(const WildcardValue& wildcard_value, const std::string& message, DeleteItemHandler handler) {
    size_t session_id = {};
    try {
      const tao::json::value request = tao::json::from_string(message);
      const auto body = request.at("body");
      session_id = request.as<size_t>("session_id");
      tao::json::value response = {{"session_id", session_id}};
      handler(boost::lexical_cast<boost::uuids::uuid>(wildcard_value), GetDeleteItemCallback(std::move(response)));
    } catch (const std::exception& ex) {
      tao::json::value response = {{"session_id", session_id}};
      response["status"] = 400;
      response["body"] = std::string("{\"error\": \"") + ex.what() + "\"}";
      mqtt_client_.Publish(root_topic_ + "/response", tao::json::to_string(response));
    }
  }

  GetItemCallback GetGetItemCallback(tao::json::value response) {
    return [mqtt_client = mqtt_client_, root_topic = root_topic_, response = std::move(response)](
        GetItemStatus status_code, const tao::json::value& object) mutable {
      switch (status_code) {
      case GetItemStatus::Success:
        response["status"] = 200;
        response["body"] = object;
        break;
      case GetItemStatus::ItemDoesntExist:
        response["status"] = 404;
        response["body"] = "{\"error\": \"Item doesn't exist\"}";
        break;
      case GetItemStatus::OperationForbidden:
        response["status"] = 403;
        response["body"] = "{\"error\": \"Operation forbidden\"}";
        break;
      case GetItemStatus::InternalError:
        response["status"] = 500;
        response["body"] = "{\"error\": \"Internal Error\"}";
        break;
      }

      mqtt_client.Publish(root_topic + "/response", tao::json::to_string(response));
    };
  }

  GetItemCallback GetGetItemsCallback(tao::json::value response) {
    return GetGetItemCallback(std::move(response));
  }

  PutItemCallback GetPutItemCallback(tao::json::value response) {
    return [mqtt_client = mqtt_client_, root_topic = root_topic_, response = std::move(response)]
    (UpdateItemStatus status_code) mutable {
      switch (status_code) {
      case UpdateItemStatus ::Success:
        response["status"] = 200;
        break;
      case UpdateItemStatus::ItemDoesntExist:
        response["status"] = 404;
        response["body"] = "{\"error\": \"Item doesn't exist\"}";
        break;
      case UpdateItemStatus::OperationForbidden:
        response["status"] = 403;
        response["body"] = "{\"error\": \"Operation forbidden\"}";
        break;
      case UpdateItemStatus::OutOfMemory:
        response["status"] = 507;
        response["body"] = "{\"error\": \"Out of memory\"}";
        break;
      }

      mqtt_client.Publish(root_topic + "/response", tao::json::to_string(response));
    };
  }

  DeleteItemCallback GetDeleteItemCallback(tao::json::value response) {
    return [mqtt_client = mqtt_client_, root_topic = root_topic_, response = std::move(response)]
        (DeleteItemStatus status_code) mutable {
      switch (status_code) {
      case DeleteItemStatus ::Success:
        response["status"] = 200;
        break;
      case DeleteItemStatus::ItemDoesntExist:
        response["status"] = 404;
        response["body"] = "{\"error\": \"Item doesn't exist\"}";
        break;
      case DeleteItemStatus::OperationForbidden:
        response["status"] = 403;
        response["body"] = "{\"error\": \"Operation forbidden\"}";
        break;
      }

      mqtt_client.Publish(root_topic + "/response", tao::json::to_string(response));
    };
  }

private:
  MqttClient& mqtt_client_;
  std::string root_topic_;
  std::string resource_name_;
};

#endif // HTTP2MQTTBRIDGE_MQTT_RESOURCE_H

#include "DataStorePrimitives+Json.h"

#include <sstream>
#include <stdexcept>
#include <variant>

#include <nlohmann/json.hpp>
#include <base64.h>
#include <uuid.h>

#include "DataStorePrimitives.h"

using json = nlohmann::json;


namespace Lichtenstein::Server::DB::Types {
/**
 * Serializes a ParamMap to json.
 */
json ParamMapToJson(const ParamMapType &m) {
    json j;

    for(auto const &[key, value] : m) {
        if(std::holds_alternative<bool>(value)) {
            j[key] = std::get<bool>(value);
        }
        else if(std::holds_alternative<double>(value)) {
            j[key] = std::get<double>(value);
        }
        else if(std::holds_alternative<uint64_t>(value)) {
            j[key] = std::get<uint64_t>(value);
        }
        else if(std::holds_alternative<int64_t>(value)) {
            j[key] = std::get<int64_t>(value);
        }
        else if(std::holds_alternative<std::string>(value)) {
            j[key] = std::get<std::string>(value);
        } else {
            std::stringstream what;
            what << "Unable to serialize type id " << value.index() 
                << " to json";
            throw std::runtime_error(what.str());
        } 
    }

    return j;
}
/**
 * Converts JSON to a ParamMap.
 */
ParamMapType JsonToParamMap(const json &j) {
    ParamMapType m;

    // iterate over each key in the json object
    for(auto it = j.begin(); it != j.end(); ++it) {
        const auto key = it.key();
        auto value = it.value();

        // insert the proper representation of it
        if(value.is_boolean()) {
            m[key] = value.get<bool>();
        } else if(value.is_number_float()) {
            m[key] = value.get<double>();
        } else if(value.is_number_unsigned()) {
            m[key] = value.get<uint64_t>();
        } else if(value.is_number_integer()) {
            m[key] = value.get<int64_t>();
        } else if(value.is_string()) {
            m[key] = value.get<std::string>();
        } else {
            std::stringstream what;
            what << "Unable to convert json value '" << value << "'";

            throw std::runtime_error(what.str());
        }
    }

    // only if we get down here did everything convert as planned
    return m;
}



/**
 * JSON (de)serialization for routines
 */
void to_json(json &j, const Routine &r) {
    j = {
        {"id", r.id},
        {"name", r.name},
        {"code", r.code},
        {"params", ParamMapToJson(r.params)},
        {"lastModified", r.lastModified}
    };
}

void from_json(const json &j, Routine &r) {
    // ID is optional when reading from json
    try {
        j.at("id").get_to(r.id);
    } catch(json::out_of_range &) {
        r.id = -1;
    }

    j.at("name").get_to(r.name);
    j.at("code").get_to(r.code);

    // params can be omitted
    try {
        r.params = JsonToParamMap(j.at("params"));
    } catch(json::out_of_range &) {}
}



/**
 * JSON (de)serialization for groups
 */
void to_json(json &j, const Group &g) {
    j = {
        {"id", g.id},
        {"name", g.name},
        {"enabled", g.enabled},
        {"start", g.startOff},
        {"end", g.endOff},
        {"brightness", g.brightness},
        {"mirrored", g.mirrored},
        {"routineId", nullptr},
        {"routineState", nullptr},
        
        {"lastModified", g.lastModified}
    };

    // is there a routine state?
    if(g.routineId) {
        j["routineId"] = *g.routineId;
        j["routineState"] = ParamMapToJson(*g.routineState);
    }
}

void from_json(const json &j, Group &g) {
    // ID is optional when reading from json
    try {
        j.at("id").get_to(g.id);
    } catch(json::out_of_range &) {
        g.id = -1;
    }

    // mandatory fields
    j.at("name").get_to(g.name);
    j.at("enabled").get_to(g.enabled);
    j.at("start").get_to(g.startOff);
    j.at("end").get_to(g.endOff);
    j.at("mirrored").get_to(g.mirrored);

    // routine id or state is _not_ input from json
} 



/**
 * JSON (de)serialization for nodes
 */
void to_json(json &j, const Node &n) {
    j = {
        {"id", n.id},
        {"label", nullptr},
        {"address", n.address},
        {"hostname", n.hostname},
        {"enabled", n.enabled},
        {"versions", json({
            {"sw", n.swVersion},
            {"hw", n.hwVersion},
        })},
        {"uuid", uuids::to_string(n.uuid)},
        {"lastCheckin", n.lastCheckin},
        {"lastModified", n.lastModified}
    };

    if(n.label) {
        j["label"] = *n.label;
    }
}

void from_json(const json &j, Node &n) {
    // ID is optional when reading from json
    try {
        j.at("id").get_to(n.id);
    } catch(json::out_of_range &) {
        n.id = -1;
    }

    // if label is omitted, set it to null
    try {
        n.label = std::make_shared<std::string>(j.at("label").get<std::string>());
    } catch(json::out_of_range &) {
        n.label = nullptr;
    }

    // get enabled flag
    j.at("enabled").get_to(n.enabled);

    // read the uuid
    auto id = uuids::uuid::from_string(j.at("uuid").get<std::string>());
    if(id.has_value()) {
        n.uuid = id.value();
    } else {
        throw std::runtime_error("Failed to parse node UUID");
    }

    // read base64-encoded secret if specified
    try {
        auto base64Str = j.at("sharedSecret").get<std::string>();
        auto data = base64_decode(base64Str);

        n.sharedSecret.clear();
        n.sharedSecret.assign(data.begin(), data.end());
    } catch(json::out_of_range &) {
        // nothing
    }
}



/**
 * JSON (de)serialization for node channels
 */
void to_json(json &j, const NodeChannel &c) {
    j = {
        {"id", c.id},
        {"nodeId", c.nodeId},
        {"label", nullptr},
        {"index", c.nodeChannelIndex},
        {"numPixels", c.numPixels},
        {"fbOffset", c.fbOffset},
        {"format", c.format},
        {"lastModified", c.lastModified}
    };

    if(c.label) {
        j["label"] = *c.label;
    }
}

void from_json(const json &j, NodeChannel &c) {
    // ID is optional when reading from json
    try {
        j.at("id").get_to(c.id);
    } catch(json::out_of_range &) {
        c.id = -1;
    }

    // if label is omitted, set it to null
    try {
        c.label = std::make_shared<std::string>(j.at("label").get<std::string>());
    } catch(json::out_of_range &) {
        c.label = nullptr;
    }

    // framebuffer offset is mandatory
    j.at("fbOffset").get_to(c.fbOffset);
}
}
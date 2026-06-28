/**
 * MCP Transport Adapter 实现
 *
 * 桥接逻辑:
 *   AgentBridge 设备注册 → McpServer::AddTool()
 *   McpServer tool call   → AgentBridge handle_tool_call()
 *
 * 依赖: 小智 McpServer (mcp_server.h)
 */

#include "transport_mcp.h"
#include "mcp_server.h"
#include <cstring>
#include <string>
#include <cJSON.h>
#include <esp_log.h>

static const char *TAG = "transport_mcp";

/* ---- 工具回调: McpServer tool call → AgentBridge ---- */

/**
 * 通用 tool call 处理:
 *   从 McpServer 的 PropertyList 中提取参数,
 *   构造 JSON arguments 字符串,
 *   调用 AgentBridge 内部处理 (通过 agent_bridge_c_handle_tool_call)
 */

/* AgentBridge 内部函数声明 (来自 agent_bridge.h) */
#ifdef __cplusplus
extern "C" {
#endif

/* 使用 agent_bridge_dispatch_tool — agent_bridge.h 中的公开 API */
/* (agent_bridge.h 声明, agent_bridge.c 实现) */

#ifdef __cplusplus
}
#endif

/* ---- 全局 bridge 指针 (单例) ---- */
static agent_bridge_t *g_bridge = nullptr;

/* ---- 导出设备到 McpServer ---- */

/**
 * 将 AgentBridge 设备列表中的所有设备注册为 McpTool
 */
void transport_mcp_export_devices(agent_bridge_t *bridge) {
    if (!bridge) return;
    g_bridge = bridge;

    auto &mcp = McpServer::GetInstance();

    /* 获取设备列表 (通过 agent_bridge 内部接口) */
    char dev_json[4096];
    agent_bridge_get_device_list_json(bridge, dev_json, sizeof(dev_json));
    ESP_LOGI(TAG, "Exporting devices: %s", dev_json);

    /* 获取 MCP tools 列表 */
    char tools_json[4096];
    agent_bridge_get_tools_json(bridge, tools_json, sizeof(tools_json));
    ESP_LOGI(TAG, "Exporting tools: %s", tools_json);

    /* 解析 tools JSON 数组, 逐一注册到 McpServer */
    cJSON *tools = cJSON_Parse(tools_json);
    if (!tools || !cJSON_IsArray(tools)) {
        ESP_LOGE(TAG, "Failed to parse tools JSON");
        return;
    }

    int tool_count = cJSON_GetArraySize(tools);
    for (int i = 0; i < tool_count; i++) {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        if (!tool) continue;

        cJSON *name_item = cJSON_GetObjectItem(tool, "name");
        cJSON *desc_item = cJSON_GetObjectItem(tool, "description");
        cJSON *schema_item = cJSON_GetObjectItem(tool, "inputSchema");

        if (!name_item || !desc_item || !schema_item) continue;

        const char *tool_name = name_item->valuestring;
        const char *tool_desc = desc_item->valuestring;

        /* 解析 inputSchema → PropertyList */
        PropertyList props;
        cJSON *properties = cJSON_GetObjectItem(schema_item, "properties");
        cJSON *required = cJSON_GetObjectItem(schema_item, "required");

        if (properties && cJSON_IsObject(properties)) {
            cJSON *prop = properties->child;
            while (prop) {
                cJSON *type_item = cJSON_GetObjectItem(prop, "type");
                const char *prop_type_str = type_item ? type_item->valuestring : "string";

                PropertyType ptype;
                if (strcmp(prop_type_str, "boolean") == 0) {
                    ptype = kPropertyTypeBoolean;
                } else if (strcmp(prop_type_str, "integer") == 0) {
                    ptype = kPropertyTypeInteger;
                } else {
                    ptype = kPropertyTypeString;
                }

                /* 检查是否有默认值 (不在 required 列表中) */
                bool has_default = true;
                if (required && cJSON_IsArray(required)) {
                    for (int r = 0; r < cJSON_GetArraySize(required); r++) {
                        cJSON *req = cJSON_GetArrayItem(required, r);
                        if (req && req->valuestring &&
                            strcmp(req->valuestring, prop->string) == 0) {
                            has_default = false;
                            break;
                        }
                    }
                }

                if (has_default) {
                    /* 有默认值的 property */
                    if (ptype == kPropertyTypeBoolean) {
                        props.AddProperty(Property(prop->string, ptype, false));
                    } else if (ptype == kPropertyTypeInteger) {
                        cJSON *min_item = cJSON_GetObjectItem(prop, "minimum");
                        cJSON *max_item = cJSON_GetObjectItem(prop, "maximum");
                        if (min_item && max_item) {
                            props.AddProperty(Property(prop->string, ptype, 0,
                                                      min_item->valueint,
                                                      max_item->valueint));
                        } else {
                            props.AddProperty(Property(prop->string, ptype, 0));
                        }
                    } else {
                        props.AddProperty(Property(prop->string, ptype,
                                                   std::string("")));
                    }
                } else {
                    /* 必需参数 */
                    if (ptype == kPropertyTypeInteger) {
                        cJSON *min_item = cJSON_GetObjectItem(prop, "minimum");
                        cJSON *max_item = cJSON_GetObjectItem(prop, "maximum");
                        if (min_item && max_item) {
                            props.AddProperty(Property(prop->string, ptype,
                                                      min_item->valueint,
                                                      max_item->valueint));
                        } else {
                            props.AddProperty(Property(prop->string, ptype));
                        }
                    } else {
                        props.AddProperty(Property(prop->string, ptype));
                    }
                }
                prop = prop->next;
            }
        }

        /* 注册到 McpServer */
        /* 需要用 lambda 捕获 tool_name */
        std::string tname(tool_name);
        mcp.AddTool(tname,
                    tool_desc ? tool_desc : "",
                    props,
                    [bridge, tname](PropertyList args) -> ReturnValue {
                        /* 将 PropertyList 转为 JSON arguments 字符串 */
                        cJSON *args_json = cJSON_CreateObject();
                        for (const auto &prop : args) {
                            if (prop.type() == kPropertyTypeBoolean) {
                                cJSON_AddBoolToObject(args_json,
                                    prop.name().c_str(), prop.value<bool>());
                            } else if (prop.type() == kPropertyTypeInteger) {
                                cJSON_AddNumberToObject(args_json,
                                    prop.name().c_str(), prop.value<int>());
                            } else {
                                cJSON_AddStringToObject(args_json,
                                    prop.name().c_str(),
                                    prop.value<std::string>().c_str());
                            }
                        }
                        char *args_str = cJSON_PrintUnformatted(args_json);
                        std::string args_s(args_str);
                        cJSON_free(args_str);
                        cJSON_Delete(args_json);

                        /* 调用 AgentBridge 执行设备操作 */
                        char result_buf[512];
                        agent_bridge_dispatch_tool(bridge, tname.c_str(), args_s.c_str(),
                                                   result_buf, sizeof(result_buf));

                        /* 返回结果 */
                        return std::string("OK");
                    });

        ESP_LOGI(TAG, "  registered MCP tool: %s", tool_name);
    }

    cJSON_Delete(tools);
    ESP_LOGI(TAG, "Exported %d tools to McpServer", tool_count);
}

int transport_mcp_attach(agent_bridge_t *bridge) {
    if (!bridge) return -1;
    g_bridge = bridge;
    transport_mcp_export_devices(bridge);
    return 0;
}

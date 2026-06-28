/**
 * MCP Transport Adapter — 连接 AgentBridge 与现有小智 McpServer
 *
 * 这层将 AgentBridge 的设备抽象映射到小智的 McpServer:
 *   agent_bridge_register_device() → 自动在 McpServer 中注册 tool
 *   McpServer 收到 tool call   → 转发到 agent_bridge handle_tool_call()
 *
 * 用法:
 *   #include "transport_mcp.h"
 *   transport_mcp_attach(bridge);  // 在 InitializeTools() 末尾调用
 */

#ifndef AGENT_TRANSPORT_MCP_H
#define AGENT_TRANSPORT_MCP_H

#include "../agent_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 将 AgentBridge 连接到现有 McpServer
 *
 * 调用后:
 *   1. agent_bridge_register_device() 会自动在 McpServer 创建对应 tool
 *   2. McpServer tool call 会自动分发到 AgentBridge 设备
 *
 * @param bridge  AgentBridge 实例
 * @return        0=成功
 */
int transport_mcp_attach(agent_bridge_t *bridge);

/**
 * 将 AgentBridge 设备列表导出到 McpServer (在 McpServer::AddCommonTools 之后调用)
 * 遍历所有已注册设备, 在 McpServer 中创建对应的 MCP tool
 */
void transport_mcp_export_devices(agent_bridge_t *bridge);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_TRANSPORT_MCP_H */

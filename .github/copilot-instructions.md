# Copilot Instructions for XSAN

<!-- Use this file to provide workspace-specific custom instructions to Copilot. For more details, visit https://code.visualstudio.com/docs/copilot/copilot-customization#_use-a-githubcopilotinstructionsmd-file -->

This is a C language distributed storage system project (XSAN) similar to VMware vSAN for KVM virtualization environments.

## Project Context
- **Language**: C (C99 standard)
- **Target Platform**: Linux (Ubuntu/CentOS/RHEL)
- **Architecture**: Distributed storage system with cluster management
- **Dependencies**: libvirt, pthread, libuuid, json-c, leveldb

## Code Style Guidelines
- Use snake_case for function and variable names
- Use UPPER_CASE for constants and macros
- Follow Linux kernel coding style guidelines
- All functions should have proper error handling
- Use consistent indentation (4 spaces, no tabs)
- Include comprehensive logging for debugging

## Core Components
1. **Cluster Management** - Node discovery, health monitoring, consensus
2. **Storage Engine** - Block allocation, data distribution, metadata management
3. **Replication** - Data redundancy, consistency, recovery
4. **Policy Engine** - Storage policies, QoS, performance optimization
5. **KVM Integration** - libvirt integration, VM storage provisioning

## Memory Management
- Always check malloc/calloc return values
- Use proper cleanup patterns with goto for error handling
- Implement reference counting for shared data structures
- Use memory pools for frequently allocated objects

## Concurrency
- Use pthread for multi-threading
- Implement proper locking strategies to avoid deadlocks
- Use atomic operations where appropriate
- Design lock-free data structures when possible

## Error Handling
- Use consistent error codes across the system
- Implement proper logging with different severity levels
- Always cleanup resources in error paths
- Provide meaningful error messages for debugging

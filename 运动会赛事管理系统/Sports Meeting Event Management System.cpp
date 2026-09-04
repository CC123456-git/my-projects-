#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#define MAX_NAME_LEN 50
#define MAX_ID_LEN 10
#define MAX_SPORT_LEN 20

// 图的节点类型枚举
typedef enum { NODE_ATHLETE, NODE_EVENT } NodeType;

// 图节点结构体（运动员或比赛）
typedef struct GraphNode {
    NodeType type;
    char id[MAX_ID_LEN];
    char name[MAX_NAME_LEN];
    union {
        char sport[MAX_SPORT_LEN];  // 运动员特有
        int max_participants;       // 比赛特有
    } attr;
    struct EdgeNode* edges;  // 邻接表边
    struct GraphNode* next;  // 下一个节点
} GraphNode;

// 图的边结构体
typedef struct EdgeNode {
    struct GraphNode* node;  // 指向的节点
    struct EdgeNode* next;   // 下一条边
} EdgeNode;

GraphNode* graph = NULL;  // 图的头节点

// 函数声明,主要是打印和输出表
void print_menu();
GraphNode* add_athlete();
GraphNode* add_event();
void assign_athlete_to_event();
void print_athletes();
void print_events();
void print_graph();
void free_memory();
GraphNode* find_node(NodeType type, const char* id_or_name);
void find_shortest_path();
void bfs_shortest_path(GraphNode* start, GraphNode* end);

int main() {
    int choice;

    printf("=== 图结构版运动会管理系统 ===\n");

    // 释放内存，退出的时候
    atexit(free_memory);

    while (1) {
        print_menu();
        printf("请选择操作: ");
        if (scanf("%d", &choice) != 1) {
            printf("输入无效，请重新输入！\n");
            while (getchar() != '\n');
            continue;
        }
        getchar();
        //根据输入的序号分配函数操作
        switch (choice) {
            case 1:
                add_athlete();
                break;
            case 2:
                add_event();
                break;
            case 3:
                assign_athlete_to_event();
                break;
            case 4:
                print_athletes();
                break;
            case 5:
                print_events();
                break;
            case 6:
                print_graph();
                break;
            case 7:
                find_shortest_path();
                break;
            case 0:
                printf("退出系统。\n");
                exit(0);
            default:
                printf("无效选择，请重试。\n");
        }
    }

    return 0;
}

void print_menu() {
    printf("\n=== 主菜单 ===\n");
    printf("1. 添加运动员\n");
    printf("2. 添加比赛项目\n");
    printf("3. 分配运动员参赛\n");
    printf("4. 查看所有运动员\n");
    printf("5. 查看所有比赛项目\n");
    printf("6. 查看完整图结构\n");
    printf("7. 查找运动员到比赛的最短路径\n");
    printf("0. 退出\n");
}
//运动员的信息
GraphNode* add_athlete() {
    GraphNode* new_node = (GraphNode*)malloc(sizeof(GraphNode));
    if (!new_node) {
        printf("内存分配失败！\n");
        return NULL;
    }

    new_node->type = NODE_ATHLETE;

    printf("请输入运动员姓名: ");
    if (fgets(new_node->name, MAX_NAME_LEN, stdin) == NULL) {
        free(new_node);
        printf("读取输入失败！\n");
        return NULL;
    }
    new_node->name[strcspn(new_node->name, "\n")] = '\0';

    printf("请输入运动员参赛项目类型: ");
    if (fgets(new_node->attr.sport, MAX_SPORT_LEN, stdin) == NULL) {
        free(new_node);
        printf("读取输入失败！\n");
        return NULL;
    }
    new_node->attr.sport[strcspn(new_node->attr.sport, "\n")] = '\0';

    // 生成ID，第一个是A001，然后累加
    static int athlete_id_counter = 1;
    snprintf(new_node->id, MAX_ID_LEN, "A%03d", athlete_id_counter++);

    new_node->edges = NULL;
    new_node->next = graph;
    graph = new_node;

    printf("成功添加运动员 %s, ID: %s\n", new_node->name, new_node->id);
    return new_node;
}
//赛事信息
GraphNode* add_event() {
    GraphNode* new_node = (GraphNode*)malloc(sizeof(GraphNode));
    if (!new_node) {
        printf("内存分配失败！\n");
        return NULL;
    }

    new_node->type = NODE_EVENT;

    printf("请输入比赛项目名称: ");
    if (fgets(new_node->name, MAX_NAME_LEN, stdin) == NULL) {
        free(new_node);
        printf("读取输入失败！\n");
        return NULL;
    }
    new_node->name[strcspn(new_node->name, "\n")] = '\0';

    printf("请输入最大参赛人数: ");
    if (scanf("%d", &new_node->attr.max_participants) != 1) {
        free(new_node);
        printf("输入无效！\n");
        while (getchar() != '\n');
        return NULL;
    }
    getchar();

    new_node->edges = NULL;
    new_node->next = graph;
    graph = new_node;

    printf("成功添加比赛项目 %s\n", new_node->name);
    return new_node;
}
//加入赛事
void assign_athlete_to_event() {
    char athlete_id[MAX_ID_LEN];
    char event_name[MAX_NAME_LEN];

    printf("请输入运动员ID: ");
    if (fgets(athlete_id, MAX_ID_LEN, stdin) == NULL) {
        printf("读取输入失败！\n");
        return;
    }
    athlete_id[strcspn(athlete_id, "\n")] = '\0';

    printf("请输入比赛项目名称: ");
    if (fgets(event_name, MAX_NAME_LEN, stdin) == NULL) {
        printf("读取输入失败！\n");
        return;
    }
    event_name[strcspn(event_name, "\n")] = '\0';

    // 查找运动员节点
    GraphNode* athlete = find_node(NODE_ATHLETE, athlete_id);
    if (athlete == NULL) {
        printf("未找到该运动员！\n");
        return;
    }

    // 查找比赛节点
    GraphNode* event = find_node(NODE_EVENT, event_name);
    if (event == NULL) {
        printf("未找到该比赛项目！\n");
        return;
    }

    // 检查运动员是否已参加该比赛
    EdgeNode* edge = event->edges;
    while (edge != NULL) {
        if (edge->node == athlete) {
            printf("该运动员已参加此比赛！\n");
            return;
        }
        edge = edge->next;
    }

    // 检查比赛是否已满
    int participant_count = 0;
    edge = event->edges;
    while (edge != NULL) {
        participant_count++;
        edge = edge->next;
    }

    if (participant_count >= event->attr.max_participants) {
        printf("该比赛项目已满员！\n");
        return;
    }

    // 检查运动员项目类型是否匹配
    if (strstr(event->name, athlete->attr.sport) == NULL) {
        printf("运动员项目类型与比赛不匹配！\n");
        return;
    }

    // 创建边（运动员->比赛）
    EdgeNode* new_edge = (EdgeNode*)malloc(sizeof(EdgeNode));
    if (!new_edge) {
        printf("内存分配失败！\n");
        return;
    }
    new_edge->node = athlete;
    new_edge->next = event->edges;
    event->edges = new_edge;

    printf("成功分配运动员 %s 参加比赛 %s\n", athlete->name, event->name);
}

GraphNode* find_node(NodeType type, const char* id_or_name) {
    GraphNode* current = graph;
    while (current != NULL) {
        if (current->type == type) {
            if ((type == NODE_ATHLETE && strcmp(current->id, id_or_name) == 0) ||
                (type == NODE_EVENT && strcmp(current->name, id_or_name) == 0)) {
                return current;
            }
        }
        current = current->next;
    }
    return NULL;
}
//输出所有运动员当前的信息
void print_athletes() {
    printf("\n=== 所有运动员信息 ===\n");
    printf("%-8s %-20s %-15s\n", "ID", "姓名", "项目类型");
    printf("--------------------------------\n");

    GraphNode* current = graph;
    while (current != NULL) {
        if (current->type == NODE_ATHLETE) {
            printf("%-8s %-20s %-15s\n",
                   current->id,
                   current->name,
                   current->attr.sport);
        }
        current = current->next;
    }
}
//输出所有比赛当前的信息
void print_events() {
    printf("\n=== 所有比赛项目信息 ===\n");
    printf("%-20s %-10s %-10s\n", "比赛名称", "当前人数", "最大人数");
    printf("----------------------------------------\n");

    GraphNode* current = graph;
    while (current != NULL) {
        if (current->type == NODE_EVENT) {
            int count = 0;
            EdgeNode* edge = current->edges;
            while (edge != NULL) {
                count++;
                edge = edge->next;
            }

            printf("%-20s %-10d %-10d\n",
                   current->name,
                   count,
                   current->attr.max_participants);
        }
        current = current->next;
    }
}
//输出完整的图结果
void print_graph() {
    printf("\n=== 完整图结构 ===\n");

    GraphNode* current = graph;
    while (current != NULL) {
        if (current->type == NODE_ATHLETE) {
            printf("运动员 %s (ID: %s, 项目: %s) 参加的比赛:\n",
                   current->name, current->id, current->attr.sport);

            // 查找所有包含该运动员的比赛
            GraphNode* event_node = graph;
            bool has_events = false;

            while (event_node != NULL) {
                if (event_node->type == NODE_EVENT) {
                    EdgeNode* edge = event_node->edges;
                    while (edge != NULL) {
                        if (edge->node == current) {
                            printf("  - %s\n", event_node->name);
                            has_events = true;
                            break;
                        }
                        edge = edge->next;
                    }
                }
                event_node = event_node->next;
            }

            if (!has_events) {
                printf("  (暂无参赛记录)\n");
            }
        }
        else if (current->type == NODE_EVENT) {
            printf("比赛 %s (最大人数: %d) 的参赛者:\n",
                   current->name, current->attr.max_participants);

            EdgeNode* edge = current->edges;
            if (edge == NULL) {
                printf("  (暂无参赛者)\n");
            }
            else {
                while (edge != NULL) {
                    printf("  - %s (ID: %s)\n", edge->node->name, edge->node->id);
                    edge = edge->next;
                }
            }
        }

        current = current->next;
        printf("\n");
    }
}
//释放内存+
void free_memory() {
    GraphNode* current_node = graph;
    while (current_node != NULL) {
        GraphNode* next_node = current_node->next;

        // 释放边
        EdgeNode* current_edge = current_node->edges;
        while (current_edge != NULL) {
            EdgeNode* next_edge = current_edge->next;
            free(current_edge);
            current_edge = next_edge;
        }

        free(current_node);
        current_node = next_node;
    }

    graph = NULL;
}

// 最短路径主函数
void find_shortest_path() {
    char athlete_id[MAX_ID_LEN];
    char event_name[MAX_NAME_LEN];

    printf("请输入运动员ID: ");
    if (fgets(athlete_id, MAX_ID_LEN, stdin) == NULL) {
        printf("读取输入失败！\n");
        return;
    }
    athlete_id[strcspn(athlete_id, "\n")] = '\0';

    printf("请输入目标比赛项目名称: ");
    if (fgets(event_name, MAX_NAME_LEN, stdin) == NULL) {
        printf("读取输入失败！\n");
        return;
    }
    event_name[strcspn(event_name, "\n")] = '\0';

    // 查找运动员和比赛节点
    GraphNode* athlete = find_node(NODE_ATHLETE, athlete_id);
    GraphNode* event = find_node(NODE_EVENT, event_name);

    if (athlete == NULL) {
        printf("未找到该运动员！\n");
        return;
    }
    if (event == NULL) {
        printf("未找到该比赛项目！\n");
        return;
    }

    printf("\n查找 %s 到比赛 %s 的最短路径...\n", athlete->name, event->name);
    bfs_shortest_path(athlete, event);
}

// 广度优先搜索实现最短路径
void bfs_shortest_path(GraphNode* start, GraphNode* end) {
    if (start == end) {
        printf("起点和终点相同，无需路径。\n");
        return;
    }

    // 队列节点定义
    typedef struct {
        GraphNode* node;
        GraphNode* parent;
    } QueueNode;

    // 队列实现（简化版，使用动态数组）
    QueueNode* queue = (QueueNode*)malloc(100 * sizeof(QueueNode));  // 假设最大队列长度100
    int front = 0, rear = 0;
    bool* visited = (bool*)calloc(100, sizeof(bool));  // 标记节点是否访问过
    GraphNode* node_list[100];  // 存储节点顺序，用于标记访问
    int node_count = 0;

    // 初始化队列
    queue[rear].node = start;
    queue[rear].parent = NULL;
    rear++;

    // 记录节点到列表
    node_list[node_count++] = start;
    visited[0] = true;

    bool found = false;
    GraphNode* current_parent = NULL;

    while (front < rear) {
        GraphNode* current = queue[front].node;
        current_parent = queue[front].parent;
        front++;

        // 检查是否到达终点
        if (current == end) {
            found = true;
            break;
        }

        // 处理运动员节点：查找其参加的所有比赛
        if (current->type == NODE_ATHLETE) {
            GraphNode* event_node = graph;
            while (event_node != NULL) {
                if (event_node->type == NODE_EVENT) {
                    EdgeNode* edge = event_node->edges;
                    while (edge != NULL) {
                        if (edge->node == current) {
                            // 检查比赛节点是否已访问
                            int idx = -1;
                            for (int i = 0; i < node_count; i++) {
                                if (node_list[i] == event_node) {
                                    idx = i;
                                    break;
                                }
                            }

                            if (idx == -1) {  // 未访问过
                                node_list[node_count++] = event_node;
                                visited[node_count - 1] = true;
                                queue[rear].node = event_node;
                                queue[rear].parent = current;
                                rear++;
                            }
                        }
                        edge = edge->next;
                    }
                }
                event_node = event_node->next;
            }
        }
            // 处理比赛节点：查找其所有运动员
        else if (current->type == NODE_EVENT) {
            EdgeNode* edge = current->edges;
            while (edge != NULL) {
                GraphNode* athlete_node = edge->node;
                // 检查运动员节点是否已访问
                int idx = -1;
                for (int i = 0; i < node_count; i++) {
                    if (node_list[i] == athlete_node) {
                        idx = i;
                        break;
                    }
                }

                if (idx == -1) {  // 未访问过
                    node_list[node_count++] = athlete_node;
                    visited[node_count - 1] = true;
                    queue[rear].node = athlete_node;
                    queue[rear].parent = current;
                    rear++;
                }
                edge = edge->next;
            }
        }
    }

    // 输出路径
    if (found) {
        printf("找到最短路径！路径如下：\n");
        GraphNode* path[100];  // 存储路径节点
        int path_len = 0;
        GraphNode* current = end;

        // 回溯路径
        while (current != NULL) {
            path[path_len++] = current;
            current = current_parent;

            // 查找current_parent对应的队列节点
            for (int i = 0; i < rear; i++) {
                if (queue[i].node == current) {
                    current_parent = queue[i].parent;
                    break;
                }
            }
        }

        // 逆序打印路径
        for (int i = path_len - 1; i >= 0; i--) {
            if (path[i]->type == NODE_ATHLETE) {
                printf("运动员 %s ", path[i]->name);
            }
            else {
                printf("比赛 %s ", path[i]->name);
            }
            if (i > 0) printf("-> ");
        }
        printf("\n路径长度：%d\n", path_len - 1);
    }
    else {
        printf("未找到从该运动员到比赛的路径！\n");
    }

    // 释放内存
    free(queue);
    free(visited);
}
//
// Created by Zikai Liu on 3/23/22.
//

#include <spawn/proc_mgmt.h>
#include <string.h>
#include <unistd.h>

#define CORE_ID_OFFSET_BIT ((sizeof(domainid_t) - sizeof(coreid_t)) * 8)
STATIC_ASSERT(CORE_ID_OFFSET_BIT == 24, "CORE_ID_OFFSET_BIT");


static int proc_node_cmp(struct proc_node *n1, struct proc_node *n2)
{
    return (n1->pid < n2->pid ? -1 : n1->pid > n2->pid);
}

RB_PROTOTYPE(proc_rb_tree, proc_node, rb_entry, proc_node_cmp)
RB_GENERATE(proc_rb_tree, proc_node, rb_entry, proc_node_cmp)

STATIC_ASSERT(sizeof(domainid_t) == 4, "sizeof(domainid_t)");
STATIC_ASSERT(sizeof(coreid_t) == 1, "sizeof(coreid_t)");
#define PID_CORE_ID_FACTOR 10000000

coreid_t pid_get_core(domainid_t pid)
{
    return (coreid_t)(pid / PID_CORE_ID_FACTOR);
}

errval_t proc_mgmt_init(struct proc_mgmt *ps)
{
    RB_INIT(&ps->running);
    LIST_INIT(&ps->free_list);
    ps->pid_upper = 1;
    ps->running_count = 0;
    return SYS_ERR_OK;
}

errval_t proc_mgmt_alloc(struct proc_mgmt *ps, struct proc_node **ret)
{
    assert(ret != NULL);

    struct proc_node *node = NULL;
    if (LIST_EMPTY(&ps->free_list)) {
        if (ps->pid_upper == PID_CORE_ID_FACTOR) {
            return PROC_MGMT_ERR_NO_AVAILABLE_PID;
        }
        node = malloc(sizeof(struct proc_node));
        if (node == NULL) {
            return LIB_ERR_MALLOC_FAIL;
        }
        node->pid = ps->pid_upper + (disp_get_core_id() * PID_CORE_ID_FACTOR);
        ps->pid_upper++;
        node->accepting_cap = 0;
    } else {
        node = LIST_FIRST(&ps->free_list);
        LIST_REMOVE(node, link);
        // Reuse node->pid
    }
    assert(node != NULL);

    RB_INSERT(proc_rb_tree, &ps->running, node);
    ps->running_count++;

    *ret = node;
    return SYS_ERR_OK;
}

struct proc_node *proc_mgmt_get_node(struct proc_mgmt *ps, domainid_t pid)
{
    struct proc_node find;
    find.pid = pid;
    return RB_FIND(proc_rb_tree, &ps->running, &find);
}

#define FIND_NODE_OR_RETURN_ERR(node)                                                    \
    struct proc_node *node = proc_mgmt_get_node(ps, pid);                                         \
    if (node == NULL) {                                                                  \
        return PROC_MGMT_ERR_PID_NOT_FOUND;                                              \
    }

errval_t proc_mgmt_delete(struct proc_mgmt *ps, domainid_t pid)
{
    FIND_NODE_OR_RETURN_ERR(node)

    RB_REMOVE(proc_rb_tree, &ps->running, node);
    ps->running_count--;

    node->name[0] = '\0';
    node->dispatcher = NULL_CAP;

    LIST_INSERT_HEAD(&ps->free_list, node, link);
    return SYS_ERR_OK;
}


errval_t proc_mgmt_get_name(struct proc_mgmt *ps, domainid_t pid, char **name)
{
    FIND_NODE_OR_RETURN_ERR(node)
    *name = strdup(node->name);
    return SYS_ERR_OK;
}

errval_t proc_mgmt_get_dispatcher(struct proc_mgmt *ps, domainid_t pid,
                                  struct capref *dispatcher)
{
    FIND_NODE_OR_RETURN_ERR(node)
    *dispatcher = node->dispatcher;
    return SYS_ERR_OK;
}

errval_t proc_mgmt_get_chan(struct proc_mgmt *ps, domainid_t pid, struct aos_chan **chan)
{
    FIND_NODE_OR_RETURN_ERR(node)
    *chan = &node->chan;
    return SYS_ERR_OK;
}

errval_t proc_mgmt_get_all_pids(struct proc_mgmt *ps, domainid_t **pids, size_t *pid_count)
{
    if (ps->running_count == 0) {
        assert(RB_EMPTY(&ps->running));
        *pids = NULL;
        *pid_count = 0;
        return SYS_ERR_OK;
    }

    domainid_t *ret = malloc(sizeof(domainid_t) * ps->running_count);
    if (ret == NULL) {
        return LIB_ERR_MALLOC_FAIL;
    }
    size_t i = 0;
    struct proc_node *node = NULL;
    RB_FOREACH(node, proc_rb_tree, &ps->running)
    {
        assert(i < ps->running_count && "ret[] overflows");
        ret[i++] = node->pid;
    }
    assert(i == ps->running_count && "running_count inconsistent");
    *pids = ret;
    *pid_count = ps->running_count;
    return SYS_ERR_OK;
}

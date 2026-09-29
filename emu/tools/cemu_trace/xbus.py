"""DuckDB views for CEMU XBUS trace analysis."""
from __future__ import annotations

WIN1_START, WIN1_END = 0xEF00, 0xEFFF
WIN2_START, WIN2_END = 0xE000, 0xE7FF
WIN3_START, WIN3_END = 0xE800, 0xEBFF
WIN4_START, WIN4_END = 0xEC00, 0xEDFF

XBUS_UNKNOWN1_ID = 0xE800
XBUS_UNKNOWN1_STATUS = 0xE806
XBUS_UNKNOWN1_LAUNCH = 0xEF3A
MBOX_DOORBELL = 0xEC12
MBOX_CTRL = 0xEC14
MBOX_RESULT = 0xEC16
MBOX_COMMAND = 0xEC80
MBOX_INDEX = 0xEC82
MBOX_VALUE = 0xEC84
SCHEDULER_START, SCHEDULER_END = 0xFD14, 0xFD1F


def _optional(columns: set[str], name: str, sql_type: str) -> str:
    if name in columns:
        return f'"{name}"'
    return f"CAST(NULL AS {sql_type})"


def create_xbus_views(connection) -> None:
    """Create schema-tolerant XBUS analysis views on the current ``trace`` view."""
    columns = {
        row[1] for row in connection.execute("PRAGMA table_info('trace')").fetchall()
    }
    access = _optional(columns, "info_access_str", "VARCHAR")
    transaction_id = _optional(columns, "info_transaction_id_i64", "BIGINT")
    phase = _optional(columns, "info_phase_str", "VARCHAR")
    deadline = _optional(columns, "info_deadline_i64", "BIGINT")
    irq_asserted = _optional(columns, "info_irq_asserted_bool", "BOOLEAN")
    irq_value = _optional(columns, "info_irq_value_i64", "BIGINT")
    result = _optional(columns, "info_result_i64", "BIGINT")

    connection.execute(f"""
        CREATE OR REPLACE VIEW xbus_accesses AS
        WITH candidates AS (
            SELECT
                seq, icount, pc, addr, size, value, detail,
                CASE
                    WHEN kind = 'xbus_access' THEN {access}
                    WHEN kind = 'mem_read' THEN 'read'
                    WHEN kind = 'mem_write' THEN 'write'
                END AS access,
                CASE
                    WHEN addr BETWEEN {WIN1_START} AND {WIN1_END} THEN 'win1_ctrl'
                    WHEN addr BETWEEN {WIN2_START} AND {WIN2_END} THEN 'win2_upload'
                    WHEN addr BETWEEN {WIN3_START} AND {WIN3_END} THEN 'win3_id_upload'
                    WHEN addr BETWEEN {WIN4_START} AND {WIN4_END} THEN 'win4_mailbox'
                END AS window_name,
                kind AS source_kind
            FROM trace
            WHERE kind IN ('xbus_access', 'mem_read', 'mem_write')
              AND (
                  addr BETWEEN {WIN2_START} AND {WIN4_END}
                  OR addr BETWEEN {WIN1_START} AND {WIN1_END}
              )
        ),
        ranked AS (
            SELECT *,
                   row_number() OVER (
                       PARTITION BY icount, pc, addr, size, value, access
                       ORDER BY seq
                   ) AS duplicate_rank
            FROM candidates
            WHERE access IS NOT NULL
        )
        SELECT seq, icount, pc, window_name, access, addr, size, value, detail,
               source_kind
        FROM ranked
        WHERE duplicate_rank = 1
    """)

    connection.execute(f"""
        CREATE OR REPLACE VIEW xbus_registers AS
        SELECT
            window_name,
            addr,
            CASE addr
                WHEN {XBUS_UNKNOWN1_ID} THEN 'XBUS_UNKNOWN1_ID'
                WHEN {XBUS_UNKNOWN1_STATUS} THEN 'XBUS_UNKNOWN1_STATUS'
                WHEN {XBUS_UNKNOWN1_LAUNCH} THEN 'XBUS_UNKNOWN1_LAUNCH'
                WHEN {MBOX_DOORBELL} THEN 'MBOX_DOORBELL'
                WHEN {MBOX_CTRL} THEN 'MBOX_CTRL'
                WHEN {MBOX_RESULT} THEN 'MBOX_RESULT'
                WHEN {MBOX_COMMAND} THEN 'MBOX_CMD'
                WHEN {MBOX_INDEX} THEN 'MBOX_INDEX'
                WHEN {MBOX_VALUE} THEN 'MBOX_VALUE'
            END AS register,
            count(*) FILTER (WHERE access = 'read') AS reads,
            count(*) FILTER (WHERE access = 'write') AS writes,
            min(seq) AS first_seq,
            max(seq) AS last_seq,
            min(icount) AS first_icount,
            max(icount) AS last_icount,
            list(DISTINCT pc ORDER BY pc)
                FILTER (WHERE pc IS NOT NULL) AS pcs,
            list(DISTINCT value ORDER BY value)
                FILTER (WHERE access = 'read') AS read_values,
            list(DISTINCT value ORDER BY value)
                FILTER (WHERE access = 'write') AS write_values
        FROM xbus_accesses
        GROUP BY window_name, addr
    """)

    connection.execute(f"""
        CREATE OR REPLACE VIEW _cemu_xbus_doorbells AS
        SELECT
            row_number() OVER (ORDER BY seq) AS transaction,
            seq AS doorbell_seq,
            lead(seq) OVER (ORDER BY seq) AS next_doorbell_seq,
            icount AS doorbell_icount,
            pc AS doorbell_pc,
            value AS doorbell
        FROM xbus_accesses
        WHERE access = 'write' AND addr = {MBOX_DOORBELL} AND (value & 1) = 1
    """)

    connection.execute(f"""
        CREATE OR REPLACE VIEW _cemu_xbus_completions AS
        SELECT
            seq, icount, pc, value AS status,
            {transaction_id} AS transaction_id,
            {phase} AS phase,
            {deadline} AS deadline,
            {irq_asserted} AS irq_asserted,
            {irq_value} AS irq_value,
            {result} AS result
        FROM trace
        WHERE kind = 'xbus_mailbox_complete'
    """)

    connection.execute(f"""
        CREATE OR REPLACE VIEW xbus_transactions AS
        SELECT
            d.transaction,
            (
                SELECT arg_min(c.transaction_id, c.seq)
                FROM _cemu_xbus_completions c
                WHERE c.seq >= d.doorbell_seq
                  AND (d.next_doorbell_seq IS NULL
                       OR c.seq < d.next_doorbell_seq)
            ) AS transaction_id,
            d.doorbell_seq,
            d.next_doorbell_seq,
            d.doorbell_icount,
            d.doorbell_pc,
            d.doorbell,
            (
                SELECT value FROM xbus_accesses a
                WHERE a.access = 'write' AND a.addr = {MBOX_CTRL}
                  AND a.seq < d.doorbell_seq
                ORDER BY a.seq DESC LIMIT 1
            ) AS ctrl,
            (
                SELECT value FROM xbus_accesses a
                WHERE a.access = 'write' AND a.addr = {MBOX_COMMAND}
                  AND a.seq < d.doorbell_seq
                ORDER BY a.seq DESC LIMIT 1
            ) AS command,
            (
                SELECT value FROM xbus_accesses a
                WHERE a.access = 'write' AND a.addr = {MBOX_INDEX}
                  AND a.seq < d.doorbell_seq
                ORDER BY a.seq DESC LIMIT 1
            ) AS index,
            (
                SELECT value FROM xbus_accesses a
                WHERE a.access = 'write' AND a.addr = {MBOX_VALUE}
                  AND a.seq < d.doorbell_seq
                ORDER BY a.seq DESC LIMIT 1
            ) AS request,
            (
                SELECT min(c.seq) FROM _cemu_xbus_completions c
                WHERE c.seq >= d.doorbell_seq
                  AND (d.next_doorbell_seq IS NULL
                       OR c.seq < d.next_doorbell_seq)
            ) AS first_completion_seq,
            (
                SELECT max(c.seq) FROM _cemu_xbus_completions c
                WHERE c.seq >= d.doorbell_seq
                  AND (d.next_doorbell_seq IS NULL
                       OR c.seq < d.next_doorbell_seq)
            ) AS last_completion_seq
        FROM _cemu_xbus_doorbells d
    """)

    connection.execute(f"""
        CREATE OR REPLACE VIEW _cemu_xbus_effect_candidates AS
        SELECT
            seq, icount, pc, 'completion' AS effect,
            addr, CAST(NULL AS VARCHAR) AS access, value,
            {phase} AS phase,
            {deadline} AS deadline,
            {irq_asserted} AS irq_asserted,
            {irq_value} AS irq_value,
            {result} AS result
        FROM trace
        WHERE kind = 'xbus_mailbox_complete'
        UNION ALL
        SELECT
            seq, icount, pc,
            CASE
                WHEN addr = {MBOX_DOORBELL} AND access = 'read' THEN 'status_poll'
                WHEN addr = {MBOX_DOORBELL} AND access = 'write' THEN 'status_clear'
                WHEN addr = {MBOX_RESULT} AND access = 'read' THEN 'result_read'
            END AS effect,
            addr, access, value,
            CAST(NULL AS VARCHAR) AS phase,
            CAST(NULL AS BIGINT) AS deadline,
            CAST(NULL AS BOOLEAN) AS irq_asserted,
            CAST(NULL AS BIGINT) AS irq_value,
            CAST(NULL AS BIGINT) AS result
        FROM xbus_accesses
        WHERE (addr = {MBOX_DOORBELL} AND access = 'read')
           OR (addr = {MBOX_DOORBELL} AND access = 'write' AND (value & 1) = 0)
           OR (addr = {MBOX_RESULT} AND access = 'read')
        UNION ALL
        SELECT
            seq, icount, pc, 'scheduler_latch' AS effect,
            addr,
            CASE
                WHEN kind IN ('mem_read', 'sfr_read') THEN 'read'
                ELSE 'write'
            END AS access,
            value,
            CAST(NULL AS VARCHAR) AS phase,
            CAST(NULL AS BIGINT) AS deadline,
            CAST(NULL AS BOOLEAN) AS irq_asserted,
            CAST(NULL AS BIGINT) AS irq_value,
            CAST(NULL AS BIGINT) AS result
        FROM trace
        WHERE kind IN ('mem_read', 'mem_write', 'sfr_read', 'sfr_write')
          AND addr BETWEEN {SCHEDULER_START} AND {SCHEDULER_END}
    """)

    connection.execute("""
        CREATE OR REPLACE VIEW xbus_effects AS
        SELECT
            d.transaction,
            t.transaction_id,
            d.doorbell_seq,
            e.seq,
            e.icount,
            e.pc,
            e.effect,
            e.addr,
            e.access,
            e.value,
            e.phase,
            e.deadline,
            e.irq_asserted,
            e.irq_value,
            e.result
        FROM _cemu_xbus_effect_candidates e
        JOIN _cemu_xbus_doorbells d
          ON e.seq >= d.doorbell_seq
         AND (d.next_doorbell_seq IS NULL OR e.seq < d.next_doorbell_seq)
        LEFT JOIN xbus_transactions t USING (transaction)
    """)

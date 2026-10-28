# Remove only the fixture directory created by copy_file_sql_ext_data.inc.
connect (file_sql_ext_cleanup,$OBMYSQL_MS0,admin,$OBMYSQL_PWD,test,$OBMYSQL_PORT);
connection file_sql_ext_cleanup;

--disable_query_log
let $FILE_SQL_HOST = query_get_value('select host_ip()',host_ip(), 1);
let $FILE_SQL_EXT_DIR = $OBSERVER_DIR/data_for_mysqltest_file_sql_ext;
let $FILE_SQL_CLEANUP_CMD = ssh $FILE_SQL_HOST 'rm -rf "$FILE_SQL_EXT_DIR"';
exec $FILE_SQL_CLEANUP_CMD;
--enable_query_log

disconnect file_sql_ext_cleanup;
connection default;

# Install the fixtures of the extra file_sql cases (data_ext/) in the observer working
# directory. *.b64 files hold binary or byte exact fixtures and are decoded in place.
connect (file_sql_ext_setup,$OBMYSQL_MS0,admin,$OBMYSQL_PWD,test,$OBMYSQL_PORT);
connection file_sql_ext_setup;

--disable_query_log
let $FILE_SQL_HOST = query_get_value('select host_ip()',host_ip(), 1);
let $FILE_SQL_EXT_DIR = $OBSERVER_DIR/data_for_mysqltest_file_sql_ext;
let $FILE_SQL_COPY_CMD = ssh $FILE_SQL_HOST 'rm -rf "$FILE_SQL_EXT_DIR" && mkdir -p "$FILE_SQL_EXT_DIR"' && scp -q $PWD/mysql_test/test_suite/file_sql/data_ext/* $FILE_SQL_HOST:"$FILE_SQL_EXT_DIR/" && ssh $FILE_SQL_HOST 'cd "$FILE_SQL_EXT_DIR" && for f in *.b64; do base64 -d "\$f" > "\${f%.b64}" && rm -f "\$f"; done';
exec $FILE_SQL_COPY_CMD;
--enable_query_log

disconnect file_sql_ext_setup;
connection default;

. /etc/clearwater/config
chronos_port=`echo $chronos_hostname | perl -p -e 's/.+:(\d+)/$1/'`
cat << EOF
define command{
        command_name    restart-chronos
        command_line    /usr/lib/nagios/plugins/clearwater-abort \$SERVICESTATE$ \$SERVICESTATETYPE$ \$SERVICEATTEMPT$ /var/run/chronos.pid 30
        }

define service{
        use                             cw-service         ; Name of service template to use
        host_name                       local_ip
        service_description             Chronos port open
        check_command                   http_ping!$chronos_port
        event_handler                   restart-chronos
        }

EOF

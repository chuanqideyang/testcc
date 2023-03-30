import plotly.graph_objects as go
import sys
from datetime import datetime
import json

tracefile = sys.argv[1]

stats = {
     "transport":{},
     "sessions":{}
}

# y1: lossrate
# y2: cwnd
# y3: throughput
# y4: rtt
# y5: utility
# y6: probe_gain

interval_start_keys = {
}

interval_done_keys = {
'base_cwnd' : 'y2',
'probe_gain' : 'y6',
'target_cwnd' : 'y2',
'send_cnt': 'y2',
'ack_cnt': 'y2',
'rtt' : 'y4',
'avg_rtt': 'y4',
'throughput' : 'y3',
'utility' : 'y5',
'loss_rate': 'y1',
}

def addSessionMetric(stats, sessionid, key, val, ts):
     if not stats["sessions"].__contains__(sessionid):
          stats["sessions"][sessionid] = {}
     if not stats["sessions"][sessionid].__contains__(key):
          stats["sessions"][sessionid][key] = {"val":[], "ts":[]}
     stats["sessions"][sessionid][key]["val"].append(val)
     stats["sessions"][sessionid][key]["ts"].append(ts)

with open(tracefile, 'r') as tracefile_f:
    create_tag  = 'create new interval:'
    done_tag = 'recv_done:'
    for line in tracefile_f.readlines():
            if line.find(create_tag) > -1:
                log_time = datetime.strptime(line[1:22], '%m/%d/%y %H:%M:%S.%f')
                conn_state_pos = line.find(create_tag) + len(create_tag)
                conn_state_json = json.loads(line[conn_state_pos:])
                #print('conn_state_json:{}'.format(json.dumps(conn_state_json)))
                for key, val in conn_state_json.items():
                     if key != 'connid' and key in interval_start_keys:
                          connid = str(conn_state_json['connid'])
                          addSessionMetric(stats, connid, key, float(val), log_time)
            if line.find(done_tag) > -1:
                log_time = datetime.strptime(line[1:22], '%m/%d/%y %H:%M:%S.%f')
                conn_state_pos = line.find(done_tag) + len(done_tag)
                conn_state_json = json.loads(line[conn_state_pos:])
                #print('conn_state_json:{}'.format(json.dumps(conn_state_json)))
                for key, val in conn_state_json.items():
                     if key != 'connid' and key in interval_done_keys:
                          connid = str(conn_state_json['connid'])
                          addSessionMetric(stats, connid, key, float(val), log_time)

fig = go.Figure()
# transport traces
for key in stats["transport"].keys():
    fig.add_trace(go.Scatter(x=stats["transport"][key]["ts"], y = stats["transport"][key]["val"], mode='lines', name='transport_' + key))
# session traces
for sessionid in stats["sessions"].keys():
     for key in stats["sessions"][sessionid].keys():
        yx = interval_start_keys[key] if interval_start_keys.__contains__(key) else interval_done_keys[key]
        fig.add_trace(go.Scatter(x=stats["sessions"][sessionid][key]["ts"], y = stats["sessions"][sessionid][key]["val"], mode='lines+markers', yaxis=yx, name=sessionid[-4:]+ key))
fig.update_layout(
    xaxis=dict(
        domain=[0.05, 0.95]
    ),
    yaxis=dict(
        title="loss_rate",
        titlefont=dict(
            color="#1f77b4"
        ),
        tickfont=dict(
            color="#1f77b4"
        )
    ),
    yaxis2=dict(
        title="cwnd",
        titlefont=dict(
            color="#ff7f0e"
        ),
        tickfont=dict(
            color="#ff7f0e"
        ),
        anchor="free",
        overlaying="y",
        side="left",
        position=0.02
    ),
    yaxis3=dict(
        title="throughput",
        titlefont=dict(
            color="#d62728"
        ),
        tickfont=dict(
            color="#d62728"
        ),
        anchor="x",
        overlaying="y",
        side="right"
    ),
    yaxis4=dict(
        title="rtt",
        titlefont=dict(
            color="#9467bd"
        ),
        tickfont=dict(
            color="#9467bd"
        ),
        anchor="free",
        overlaying="y",
        side="right",
        position=0.97
    ),
    yaxis5=dict(
        title="utility",
        titlefont=dict(
            color="#95687d"
        ),
        tickfont=dict(
            color="#95687d"
        ),
        anchor="free",
        overlaying="y",
        side="right",
        position=0.99
    ),    
    yaxis6=dict(
        title="probe_gain",
        titlefont=dict(
            color="#fadf7e"
        ),
        tickfont=dict(
            color="#fadf7e"
        ),
        anchor="free",
        overlaying="y",
        side="left",
        position=0.01
    ),
)
fig.show()

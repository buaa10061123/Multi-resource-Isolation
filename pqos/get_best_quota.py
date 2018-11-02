from sklearn.externals import joblib
import decimal

###CONST###
CPU_MAX=2
CPU_MIN=0.6
CPU_STEP=0.1
MEM_MAX=102400
MEM_MIN=10240
MEM_STEP=1024
MEMBW_MAX=100
MEMBW_MIN=10
MEMBW_STEP=10
LLC_MAX=11264
LLC_MIN=1024
LLC_STEP=1024
TASKS=130
IPS = 3500
minCostFunction = None
model = None



def drange(x, y, step):
  while x > y:
    yield float(x)
    x -= decimal.Decimal(step)

def minCost_mysql(cpu,mem,llc,membw):
    if mem >= 20480:
        mincost = (cpu * 3 -2) + (mem/1024) + (llc/1024) + (membw/10)
    else:
        mincost = (cpu * 3 - 2) + (mem / 102.4) + llc / 1024 + membw / 10
    return mincost

def minCost_KafkaProduce(cpu,mem,llc,membw):
    if cpu < 1:
        cpu = cpu * 5
    if mem < 51200:
        mem = mem * 5
    mincost = cpu + mem/1024 + llc/1024 + membw/10
    return mincost

def get_quota_from_ipc():
    #model = joblib.load('XGBoostR-1024-kafkaProduce.model')
    count = 0
    threads = TASKS

    min_cost = minCostFunction(CPU_MAX,MEM_MAX,LLC_MAX,MEMBW_MAX)

    best_quota = (CPU_MAX, MEM_MAX, LLC_MAX, MEMBW_MAX)
    for cpu in drange(CPU_MAX,CPU_MIN,CPU_STEP):
        # if model.predict([[cpu, MEM_MAX, LLC_MAX, MEMBW_MAX, threads]])[0] < IPS:
        #     break
        for mem in drange(MEM_MAX,MEM_MIN,MEM_STEP):
            # if model.predict([[cpu, mem, LLC_MAX, MEMBW_MAX, threads]])[0] < IPS:
            #     break
            for llc in drange(LLC_MAX, LLC_MIN, LLC_STEP):
                if model.predict([[cpu,mem,llc,MEMBW_MAX, threads]])[0] < IPS:
                    break
                for membw in drange(MEMBW_MAX,MEMBW_MIN,MEMBW_STEP):
                    count = count+1
                    tmp = model.predict([[cpu,mem,llc,membw,threads]])[0]
                    #print(tmp,'\n')
                    if tmp < IPS:
                        break
                    else:
                        cur_cost = minCostFunction(cpu,mem,llc,membw)
                        if cur_cost < min_cost:
                            min_cost = cur_cost
                            #print(min_cost)
                            best_quota = (float('%.2f' % cpu),mem,llc,membw)
    #print(count)
    #print(IPS,TASKS)
    print(best_quota)
    return best_quota

def get_mysql_quota(ips,tasks):
    init_mysql_const(ips,tasks)
    best_quota = get_quota_from_ipc()
    return best_quota


def init_mysql_const(ips,tasks):
    # cpu:6cores mem:500MB llc:11MB mbw:100%
    global CPU_MAX
    global CPU_MIN
    global CPU_STEP
    global MEM_MAX
    global MEM_MIN
    global MEM_STEP
    global MEMBW_MAX
    global MEMBW_MIN
    global MEMBW_STEP
    global LLC_MAX
    global LLC_MIN
    global LLC_STEP
    global TASKS
    global IPS
    global model
    global minCostFunction
    CPU_MAX = 6
    CPU_MIN = 0.6
    CPU_STEP = 0.1
    MEM_MAX = 1024*500
    MEM_MIN = 10240
    MEM_STEP = 1024
    MEMBW_MAX = 100
    MEMBW_MIN = 10
    MEMBW_STEP = 10
    LLC_MAX = 11264
    LLC_MIN = 1024
    LLC_STEP = 1024

    TASKS = tasks
    # 如果未启动mysql任务，默认为100client对应的IPS
    if tasks == 0:
        IPS = 4150 * 0.9
    else:
        IPS = mysql_90_ips(tasks)
    model = joblib.load('Mysql_25&100_GBRT_1101.model')
    minCostFunction = minCost_mysql


#method to get 90% of max ips
#100clients: max = 4150 tasks=127
#25 clients: max = 3500 tasks=52
def mysql_90_ips(tasks):
    k = (4150 - 3500)/(127 - 52)
    b = 3500 - 52 * k
    return (k * tasks + b) * 0.9

# best_quota = get_quota_from_ipc(2080)
# print(best_quota)

#get_mysql_quota(0,127)
#print(model.predict([[4,40000,11264,100]])[0])

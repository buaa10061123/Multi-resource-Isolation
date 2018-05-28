from sklearn.externals import joblib
import decimal

###CONST###
CPU_MAX=4
CPU_MIN=0.6
CPU_STEP=0.2
MEM_MAX=60000
MEM_MIN=16384
MEM_STEP=2048
MEMBW_MAX=100
MEMBW_MIN=10
MEMBW_STEP=10
LLC_MAX=11264
LLC_MIN=1024
LLC_STEP=1024


model=joblib.load('GBRT.model')

def drange(x, y, step):
  while x > y:
    yield float(x)
    x -= decimal.Decimal(step)

def get_quota_from_ipc(ipc):
    count = 0
    min_cost = (CPU_MAX * 3 - 2) + (MEM_MAX / 1024 * 0.23 - 3.8)  + LLC_MAX / 1024 + MEMBW_MAX / 10
    best_quota = (CPU_MAX, MEM_MAX, LLC_MAX, MEMBW_MAX)
    for cpu in drange(CPU_MAX,CPU_MIN,CPU_STEP):
        if model.predict([[cpu, MEM_MAX, LLC_MAX, MEMBW_MAX]])[0] < ipc:
            break
        for mem in drange(MEM_MAX,MEM_MIN,MEM_STEP):
            if model.predict([[cpu, mem, LLC_MAX, MEMBW_MAX]])[0] < ipc:
                break
            for llc in drange(LLC_MAX, LLC_MIN, LLC_STEP):
                if model.predict([[cpu,mem,llc,MEMBW_MAX]])[0] < ipc:
                    break
                for membw in drange(MEMBW_MAX,MEMBW_MIN,MEMBW_STEP):
                    count = count+1
                    tmp = model.predict([[cpu,mem,llc,membw]])[0]
                    #print(tmp,'\n')
                    if tmp < ipc:
                        break
                    else:
                        cur_cost = (cpu*3-2)+(mem/1024*0.23-3.8)+llc/1024+membw/10
                        if cur_cost < min_cost:
                            min_cost = cur_cost
                            print(min_cost)
                            best_quota = (cpu,mem,llc,membw)
    print(count)
    return best_quota

best_quota = get_quota_from_ipc(0.4)
print(best_quota)

#print(model.predict([[1.4,16992-2048,11000,100]])[0])

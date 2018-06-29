import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import scipy as sp
from sklearn.metrics import mean_squared_error
from sklearn.metrics import mean_absolute_error
from sklearn.metrics import median_absolute_error
from sklearn.metrics import r2_score
from sklearn.externals import joblib

from sklearn.model_selection import train_test_split

from sklearn.preprocessing import StandardScaler

#加载数据
data = "/Users/quximing/Desktop/Muses_output.csv"
# df = pd.read_excel(data, header=0, parse_cols="A:E")
df = pd.read_csv(data, header=0, usecols=[0,1,2,3,4])


df_update = df[df.CPU > 0]
#df_update = df_update[(df.IPS/df.CPU < 0.5)]
print("Rows of df: %d, Rows of df_update: %d \n" % (len(df.index),len(df_update.index)))
#print(df_update)
import types
#df_update['IPC/CPU'] = df_update['IPC']/df_update['CPU']
#print(df_update)

#import seaborn as sns
#from scipy import stats
#sns.distplot(df_update['IPC/CPU'], rug=True, bins=20, kde=True)
#sns.kdeplot(df_update['IPC/CPU'])

#sns.distplot(df_update['IPC/CPU'], kde=False, fit=stats.invgauss)

#plt.legend()
#plt.show()

####get ipc/cpu gaussian distribution
# from scipy.stats import gaussian_kde
# kde = gaussian_kde(df_update['IPC/CPU'])
# pdf = kde.pdf(0.35)
# print(pdf)


X = np.asarray(df_update.values[:, 1:5])
y = np.asarray(df_update.values[:, 0])

X_train, X_test, y_train, y_test = train_test_split(X, y, test_size = 0.3 , random_state= 0)

####正规化
# sc = StandardScaler()
# sc.fit(X)
# X_train_std = sc.transform(X_train)
# X_test_std = sc.transform(X_test)

X_train_std = X_train
X_test_std = X_test

###########2.回归部分##########
def try_different_method(model):
    model.fit(X_train_std,y_train)
    score = model.score(X_test_std, y_test)
    result = model.predict(X_test_std)

    # Plot feature importance
    #feature_importance = model.feature_importances_
    # make importances relative to max importance
    #feature_importance = 100.0 * (feature_importance / feature_importance.max())

    #print(feature_importance)

    print('RMSE=%.3f, MSE=%.5f, MAE=%.3f, median_absolute_error=%.3f, R2=%.4f, model.score=%.4f \n'
          % (np.sqrt(mean_squared_error(y_test, result)), mean_squared_error(y_test, result), mean_absolute_error(y_test, result),
             median_absolute_error(y_test, result), r2_score(y_test, result), score))

    # plt.figure()
    # plt.plot(np.arange(len(result)), y_test,'go-',label='true value')
    # plt.plot(np.arange(len(result)),result,'ro-',label='predict value')
    # plt.title('score: %f'%score)
    # plt.legend()
    # plt.show()
    joblib.dump(model,'GBRT.model')


###########3.具体方法选择##########
####3.1决策树回归####
#from sklearn import tree #0.30
#model_DecisionTreeRegressor = tree.DecisionTreeRegressor()
####3.2线性回归####
from sklearn import linear_model #0.42
model_LinearRegression = linear_model.LinearRegression()
####3.3SVM回归####
#from sklearn import svm #0.03
#model_SVR = svm.SVR()
####3.4KNN回归####
#from sklearn import neighbors #0.23
#model_KNeighborsRegressor = neighbors.KNeighborsRegressor()
####3.5随机森林回归####
from sklearn import ensemble #0.60
model_RandomForestRegressor = ensemble.RandomForestRegressor(n_estimators=100)#这里使用20个决策树
####3.6Adaboost回归####
from sklearn import ensemble #0.54
model_AdaBoostRegressor = ensemble.AdaBoostRegressor(n_estimators=50)#这里使用50个决策树
####3.7GBRT回归####
from sklearn import ensemble #0.65
model_GradientBoostingRegressor = ensemble.GradientBoostingRegressor(n_estimators=100, verbose=1)#这里使用100个决策树
####3.8Bagging回归####
from sklearn.ensemble import BaggingRegressor #0.59
model_BaggingRegressor = BaggingRegressor()
####3.9ExtraTree极端随机树回归####
#from sklearn.tree import ExtraTreeRegressor #0.31
#model_ExtraTreeRegressor = ExtraTreeRegressor()
#岭回归
#from sklearn.linear_model import Ridge #0.41
#model_RidgeRegressor = Ridge()

####xgboostRegression####
from xgboost import XGBRegressor
model_XGBRegressor = XGBRegressor(max_depth=6, learning_rate=0.1, n_estimators=160, silent=0, alpha=3, objective='reg:linear',eval_metric='mae')

####ElasticNet####
from sklearn.linear_model import ElasticNet
model_ElasticNet = ElasticNet()

###########4.具体方法调用部分##########
try_different_method(model_GradientBoostingRegressor)

# model=joblib.load('GBRT.model')
# print(model.predict([[2336 ,1,10,10000]]))

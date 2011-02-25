from django.conf.urls.defaults import *

urlpatterns = patterns('',
    (r'^hwdetect/$', 'userreport.views_private.report_hwdetect'),
    (r'^messages/$', 'userreport.views_private.report_messages'),
    (r'^profile/$', 'userreport.views_private.report_profile'),
    (r'^user/([0-9a-f]+)$', 'userreport.views_private.report_user'),
)

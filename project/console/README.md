# Simple-FFmplay-console

# ʹ�÷�������:

    ��ʽһ:����ý���ļ�·��

    ��ʽ��:ֱ����ַ����:

        �������� rtmp://58.200.131.2:1935/livetv/hunantv
        �������� ��rtmp://58.200.131.2:1935/livetv/gxtv
        �㶫���ӣ�rtmp://58.200.131.2:1935/livetv/gdtv
        �������ӣ�rtmp://58.200.131.2:1935/livetv/dftv

    ��ʽ��:��ȡ�����豸
        ./ffmpeg -f dshow -list_devices true -i dummy

        ���������豸: "Logitech HD Webcam C270"

        ����cameraͼ����Ϣ������
        -f dshow -i video="Logitech HD Webcam C270"


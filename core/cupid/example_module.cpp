#include "cupid.h"
#include "callback.h"
#include "enums.h"
#include "basetool.h"

void TCORE::ImportModule(){
	HttpCallback example = [](const HttpRequest &r)->const HttpResponse{
		HttpResponse res;
		HttpPacketVisitor<HttpResponse> vis(res);
		vis.SetVersion("HTTP/1.1");
		vis.SetStatus(HTTP_SC_OK);
		vis.SetStatement(GetStatusCodeInfo(HTTP_SC_OK));
		const std::string body = "Hello World !";
		vis.SetHeader("Content-Length", tostring(body.size()));
		vis.SetBody(body);
		return res;
	};
	CallbackManager<HttpCallback>::GetInstance().Set("/example", example);
}

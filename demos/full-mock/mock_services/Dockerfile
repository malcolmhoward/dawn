# Mock services container — runs HomeAssistantMock and LLMHTTPServer
# from the E.C.H.O. simulation framework.

FROM python:3.12-slim

WORKDIR /opt/mock-services

# Install simulation framework
COPY simulation_framework/ /opt/simulation_framework/
RUN pip install --no-cache-dir -e "/opt/simulation_framework[all]"

# Copy mock service entrypoints
COPY demos/full-mock/mock_services/ /opt/mock-services/

CMD ["python", "entrypoint.py"]
